# Aether

A data-oriented, job-based game engine targeting Vulkan/DirectX 12. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) *(coming later)* for the full
design.

The plan for growing Aether into a full engine (editor, visual scripting /
Blueprints, text scripting, animation, audio, runtime UI, packaging, ...) is in
[`docs/ROADMAP.md`](docs/ROADMAP.md); all planning and design docs are listed in
[`docs/README.md`](docs/README.md).

## Status: Phases 8–9 and 11–13 done on the engine side (asset system; prefabs and scheduling; Luau scripting with debugger and code editor; Blueprints with compiler, VM, debugger and editor; colliders, layers, contact events, queries and character movement), Phase 10 up to its platform backends; editor window hookups and Win32/XInput input pending a Windows build. Phases 14–16 (the unified renderer; materials; animation with graphs, IK and editors) up to their GPU parts. Phase 17 done (audio: mixing, 3D, streaming, sound cues, components, device output and editors). Phase 18 done on the engine and portable-editor side (runtime game UI: widgets, controls, themes, layouts, binding, Widget Blueprints, animations, SDF text, world-space UI and the UI Designer). Phase 19 done on the engine and portable-editor side (VFX: emitters, rendering data, events and sub-emitters, components and Blueprint nodes, GPU compute codegen and the particle editor). Phase 20 done on the engine and portable-editor side (AI and navigation: navmesh baking and path queries; runtime obstacles, area volumes, off-mesh links and scene components; NavAgents on a Detour crowd with Blueprint and Luau nodes; Blackboards and Behavior Trees; AI perception; debug drawing, the Behavior Tree editor and debugger, and the Navigation panel). Phase 21 done on the engine and portable-editor side (world building: heightmap terrain, splatmaps and brushes, terrain rendering data, foliage, splines, world partition with cell streaming and a floating origin, and the terrain, foliage, spline and partition editors). Phase 22 done on the engine and portable-editor side (networking: a UDP transport with reliable, unreliable and sequenced channels; delta-snapshot replication with relevancy and a bandwidth budget; RPCs from C++, Blueprints and Luau; client-side prediction and snapshot interpolation; sessions with LAN discovery and lobbies; networked Play-in-Editor and a network profiler). Phase 23 done on the engine and portable-editor side (developer tools: console variables and the console; the profiler with Tracy forwarding and its panel; debug drawing and stat overlays; crash reports and the crash reporter; headless functional tests with JUnit reports). Phase 24 done (cross-platform: CI on Linux, macOS and Windows; a packaged Windows editor; a portable GLFW window and input layer; the Vulkan backend on Linux and macOS (MoltenVK) with headless rendering tests; the editor shell on Vulkan; ARM via sse2neon; platform plugins for Android and consoles). Phase 25 done (build, cook and package: .apak archives with LZ4/zstd and a virtual file system; the cooker; block-compressed textures; the player; packaging from the editor; patches, DLCs and encryption; ASTC, precompiled shaders and Blueprint bytecode, and the player drawing the scene, still to come). Phase 26 in progress (plugins; project templates; the player runs scripts, Blueprints and input; the editor extensibility API; the manual and a generated API reference; version control badges in the Content Browser; the 2D toolkit's sprites, tilemaps and pixel-perfect camera). Phase 27 in progress (the sequencer: level sequences, the sequence player, event, visibility, spawn, camera cut, audio and animation tracks, the sequence component, the player running sequences, and the sequencer editor)

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

### Follow-up: Unified Cross-API Renderer

The RHI's last major gap — unifying shader/pipeline/draw-call recording
across backends — now has a real, narrow implementation. `IDevice` gained
`CreatePipeline(PipelineDesc, ISwapChain&)`, and `ICommandList` gained
`BeginRenderPass`/`EndRenderPass`/`BindPipeline`/`SetPushConstants`/`Draw`.
Unlike the triangle/cube demos (which each pick between two hand-written
backend-specific implementations at startup), `rhi_demo`'s new
`AETHER_RHI_DEMO_DRAW_UNIFIED=1` mode calls these five abstract methods with
**zero `if (backend == ...)` branching** in the recording code, and produces
the same rotating triangle on both D3D12 and Vulkan — confirmed by
screenshotting both and comparing pixel-for-pixel, not just by neither
backend crashing.

What makes this tractable — and honestly, narrow — is `PipelineDesc`: one
HLSL source compiled two ways internally (`CompileHLSL`/`CompileHLSLToSPIRV`,
the same split the triangle demo already used, just moved behind the RHI),
one push-constant block, no vertex buffers, no textures/descriptors. On
D3D12 this is a root signature + PSO; on Vulkan, `VulkanSwapChain` now owns
a `VkRenderPass` and one framebuffer per image internally (created once,
recreated alongside the image views on resize) so `BeginRenderPass` can hide
the render-pass/framebuffer machinery entirely. A real material/mesh system
(vertex buffers, descriptor/texture binding, blend states, ...) is still a
distinctly larger, separate piece of work, done via `NativeHandle()` and
backend-specific recording — `rhi_demo`'s cube mode is exactly that.

**Two real bugs found by actually screenshotting both backends side by
side**, not by inspection:

- A stride bug in every screenshot-capture helper in this repo (`rhi_demo`,
  `pbr_demo`, `gltf_demo`): `ID3D12Device::GetCopyableFootprints`'s
  `pRowSizeInBytes` output parameter is the *unpadded* row size, not the
  actual byte stride between rows in the copied buffer
  (`footprint.Footprint.RowPitch`, 256-byte aligned) — using the former as a
  stride sheared the image diagonally. Invisible at `pbr_demo`'s 1280x720 or
  `gltf_demo`'s 1024x768, where the unpadded row size already happens to be
  256-aligned (5120 and 4096 respectively); `rhi_demo`'s 800x600 isn't
  (3200), which is what finally exposed it. Fixed in all three.
- D3D12 and Vulkan disagree on which way NDC +Y points. A shader-side
  push-constant flip was tried first and broke Vulkan rendering outright —
  pipeline creation "succeeded" but nothing rasterized, for a reason not
  fully root-caused, on a driver with no validation layers to explain why.
  Fixed properly instead with a negative-height Vulkan viewport (the
  standard trick, core since Vulkan 1.1) inside `BeginRenderPass` — no
  shader or push-constant changes needed, and it revealed that the
  *original* triangle mode (not just Unified) had been rendering mirrored
  between backends all along, just never caught because nothing had
  compared its two backends' pixels directly before this round's screenshot
  tooling existed.

### Follow-up: Unified renderer — real vertex buffers + textures

The Unified Cross-API Renderer above was deliberately narrow: procedural
vertices, one push-constant block, no vertex buffers or textures. This
follow-up closes exactly that gap, still with zero backend branching in
`rhi_demo`'s recording code. `IDevice` gained `CreateVertexBuffer`/
`CreateIndexBuffer` (host-visible/upload-heap on both backends) and
`CreateTexture` (uploads into a device-global bindless texture table);
`ICommandList` gained `BindVertexBuffer`/`BindIndexBuffer`/`DrawIndexed`/
`BindBindlessTextures`; `PipelineDesc` gained `use_vertex_buffer` (a single
fixed `{float3 position; float2 uv;}` layout — not a general attribute-list
API, the same "minimal shape that makes it real" trade-off `PipelineDesc`
already made) and `enable_bindless_textures`.

- **Bindless texture table**: a fixed `kMaxBindlessTextures = 32` capacity on
  both backends, sidestepping dynamic Vulkan descriptor-set-layout growth
  entirely — the layout (and a 1x1 white dummy texture, duplicated into
  every slot up front) is built once in each backend's constructor, so a
  pipeline created with `enable_bindless_textures` always sees a stable,
  fully-populated descriptor layout, never an uninitialized slot. D3D12
  reuses `gfx::DescriptorHeap`/`gfx::Texture` directly (the same bindless
  machinery `sandbox`/`pbr_demo`/`gltf_demo` already use); Vulkan builds the
  equivalent from raw `VkImage`/`VkDescriptorSet` calls, since no existing
  helper covered that in this codebase yet — a real, staged (not host-visible)
  upload this time, since texture-sampling performance is the actual point.
- **Real bug found by actually screenshotting both backends** (not by
  inspection): the rotating textured quad's rotation was permanently stuck
  at zero on Vulkan while D3D12 rendered it correctly — caught by comparing
  screenshots at frame 10 and frame 120 and finding them pixel-identical on
  Vulkan (i.e. genuinely frozen, not just slow). Root cause: DXC only
  recognizes the `[[vk::push_constant]]` attribute on a global variable of
  *struct* type — a plain HLSL `cbuffer` doesn't qualify — so without it, a
  `cbuffer` silently compiles to an ordinary Vulkan uniform-buffer descriptor
  instead of an actual push-constant block. No validation layer exists in
  this environment to flag the mismatch (see `VulkanDevice`'s class comment),
  so it just read back zeroed/garbage memory from an unbound descriptor
  slot — the texture index happened to read back as 0 too, which
  coincidentally *was* the correct index (the only texture created), making
  the bug invisible in the rendered texture and visible only in the frozen
  rotation. Fixed by switching to `[[vk::push_constant]] ConstantBuffer<T>`
  (the attribute's actually-supported form) — but `[[vk::...]]` attribute
  syntax is a hard parse error under fxc (the D3D12/`D3DCompile` path's
  compiler), not a harmless ignore, so it can't appear unconditionally in
  HLSL shared between both compilers. Guarded with `#ifdef __spirv__`
  (a macro DXC predefines only when compiling with `-spirv`, never fxc) so
  the C preprocessor strips the attribute before either compiler's parser
  ever sees it on the branch that doesn't apply.
- Two register spaces, not one, for the texture array and sampler
  (`Texture2D g_Textures[32] : register(t0, space0)` /
  `SamplerState g_Sampler : register(s0, space1)`): DXC's default HLSL→
  SPIR-V binding assignment is `binding = register number, set = space
  number`, *regardless of the resource's type letter* — so a texture array
  at `t0` and a sampler at `s0` in the *same* space would both map to
  Vulkan `(set=0, binding=0)` and collide (illegal SPIR-V: two descriptors
  can't share a binding). Putting the sampler in `space1` puts it in a
  second Vulkan descriptor set instead, with zero effect on the D3D12 root
  signature (which already tracks `(register, space)` per resource type
  independently, via the static sampler's own `RegisterSpace = 1`).

Verified visually: `rhi_demo`'s `AETHER_RHI_DEMO_DRAW_UNIFIED=1` mode now
renders a rotating, checkerboard-textured quad — a real GPU vertex/index
buffer pair and a real uploaded GPU texture sampled through the bindless
table — pixel-identical between D3D12 and Vulkan screenshots. 75/75 unit
tests still pass (this follow-up is GPU pipeline/shader plumbing, not new
unit-testable pure logic); Clear/Triangle/Cube modes regression-checked on
both backends, plus 6 quick runs + one 2000-frame run per backend and a
programmatic-resize test, all stable.

### Follow-up: Unified renderer — depth buffer + blend states

The last two gaps `PipelineDesc` left to backend-specific code: real depth
testing and alpha blending, both now toggleable per-pipeline
(`depth_test`/`enable_blending`) with zero backend branching in `rhi_demo`.

- **A depth buffer on both backends, always present.** Rather than make
  depth attachment presence itself a per-pipeline, per-render-pass
  variable — which Vulkan's rigid render-pass/framebuffer model makes
  awkward, since a framebuffer's attachment list is fixed at creation —
  both backends now unconditionally maintain one shared depth buffer per
  swap chain (`D3D12SwapChain`'s own `D32_FLOAT` resource + DSV;
  `VulkanSwapChain`'s single default render pass gained a second,
  always-declared depth attachment, with a matching depth image recreated
  alongside the color images on resize). `BeginRenderPass` always clears and
  binds it on both backends; `PipelineDesc::depth_test` is what actually
  opts a given pipeline's own `DepthStencilState`/
  `VkPipelineDepthStencilStateCreateInfo` in or out of testing/writing
  against it — a pipeline with `depth_test = false` just never reads or
  writes what's bound, exactly like every pre-existing Unified-mode pipeline
  behaved before this field existed.
- **Alpha blending**: standard non-premultiplied `src.rgb*src.a +
  dst.rgb*(1-src.a)` color blending (output alpha passes straight through
  unblended) when `enable_blending = true`, identical
  `D3D12_BLEND_SRC_ALPHA`/`VK_BLEND_FACTOR_SRC_ALPHA` factors on both
  backends.

`rhi_demo`'s Unified mode gained two more pipelines (same shared HLSL
source and vertex/index data shape, differing only in these two flags) and
a small static scene proving both, drawn alongside the existing rotating
quad:

- A red quad and a blue quad at the same screen position, blue nearer
  (`depth=0.2`) than red (`depth=0.8`) — but drawn in *reverse* depth order
  (blue first, red second). A broken or absent depth test would let the
  later, farther draw (red) incorrectly win; a working one keeps blue on top
  regardless of draw order. This is deliberately a stronger test than "draw
  near-to-far and see the right thing" would be, since draw order and depth
  order agreeing by construction wouldn't distinguish a real depth test from
  no depth test at all.
- A translucent green quad (`opacity=0.5`, blended pipeline), offset to
  overlap the right half of the red/blue pair and closer than both
  (`depth=0.1`, so it passes the depth test against them).

**Verified visually**: a screenshot shows blue fully covering the
same-position red (confirming the depth test, not just "didn't crash" —
if it were broken, red would be visibly on top instead, since it's drawn
second), a visibly blended blue-green region exactly where the green quad
overlaps blue, and pure green where it doesn't — pixel-identical between
D3D12 and Vulkan. 81/81 unit tests pass (unchanged — GPU pipeline/shader
plumbing, not new unit-testable pure logic); Clear/Triangle/Cube/Unified
modes regression-checked on both backends, 6 quick runs + one 2000-frame run
per backend, and a programmatic-resize test (exercising the depth buffer's
own recreate-on-resize path) — all stable.

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

### Follow-up: normal mapping + multiple colored lights

`pbr_demo` gained tangent-space normal mapping and four independent colored
point lights, replacing the original single directional light.

- **Normal mapping**: the sphere's tangent is derived analytically (the
  partial derivative of the sphere's position with respect to longitude),
  not looked up from a table, and re-orthogonalized per-pixel against the
  interpolated normal (Gram-Schmidt) before building the TBN basis — linear
  interpolation across a triangle doesn't preserve perpendicularity, so
  skipping that step would introduce a visible per-triangle-facet artifact.
  The normal map itself is generated procedurally (`GenerateBumpNormalMap`):
  a height field `h(u,v) = sin(u)*cos(v)` differentiated in closed form into
  a tangent-space normal (`(-dh/dx, -dh/dy, 1)`, normalized) — no image file
  or finite-difference pass needed since `h`'s derivative has an exact
  analytic form.
- **Multiple colored lights**: four point lights (inverse-square falloff)
  at the grid's corners, each a distinct saturated color, summed per-pixel
  in an unrolled shader loop — replacing the single hardcoded directional
  light. Genuinely visible in the rendered output: each corner of the
  material grid visibly picks up its nearest light's tint.

### Follow-up: image-based lighting + environment reflections

`pbr_demo`'s ambient term is no longer a flat constant — it now comes from a
procedural sky environment cubemap, sampled two ways:

- **Diffuse IBL**: a genuine diffuse-irradiance convolution
  (`GenerateIrradianceCubeFaces`), matching the standard derivation (same
  "textbook reference" approach as the BRDF and normal-map math elsewhere in
  this demo) — for each output texel's direction, a cosine-weighted Riemann
  sum over the hemisphere (64×16 samples), evaluated directly against the
  analytic `SkyColor()` function rather than by texture-sampling a generated
  cube (mathematically the same integral, since `SkyColor` *is* the
  environment's radiance function — this just skips a redundant round trip
  through a texture). Computed once at startup at low resolution (16×16 per
  face): diffuse irradiance is a low-frequency function by construction, so
  there's no detail lost by not computing it at a higher resolution.
- **Environment reflections**: full split-sum prefiltered specular IBL (see
  the follow-up below) — roughness-correct, not just roughness-faded.

**Verified numerically, not just visually**, since the effect is subtle when
blended with a saturated albedo and bright point lights: sampled pixels
directly from a smooth dielectric sphere (the case where the specular term
is least tinted by albedo — dielectrics have an achromatic F0) show the blue
channel exceeding green near the top of the sphere (reflecting the sky's
zenith) and dropping below it near the bottom (reflecting the ground tone) —
the expected directional variation, confirmed with actual pixel values, not
assumed from the code looking right.

### Follow-up: full split-sum prefiltered specular IBL

The roughness-faded direct-reflection stand-in above is replaced with the
real split-sum approximation (Karis, "Real Shading in Unreal Engine 4",
2013) — the same technique behind every modern realtime PBR renderer's IBL:

- **Prefiltered environment mip chain** (`GeneratePrefilteredEnvironmentMip`,
  `kEnvMapMipCount = 5`): mip 0 is the sharp mirror environment (roughness
  0), and each subsequent mip is GGX-importance-sampled (`Hammersley` +
  `ImportanceSampleGGXTangent`, the same low-discrepancy-sequence technique
  the LearnOpenGL/Karis reference derivation uses) against progressively
  higher roughness, at progressively lower resolution — a rougher lobe is a
  lower-frequency function of direction, exactly like the diffuse-irradiance
  map's low resolution above, so less resolution loses no real detail.
  Sample counts scale up as resolution drops (1/32/64/128/256 across the 5
  mips) so quality stays roughly constant per output texel within the same
  startup-time budget. `CreateCubemapTexture` was generalized from a
  single-mip cubemap uploader to a multi-mip one (`mip + face * mip_count`
  D3D12 subresource indexing) to hold the chain.
- **BRDF LUT** (`GenerateBRDFLUT`/`IntegrateBRDF`): a 128×128 2D texture,
  indexed by `(NdotV, roughness)`, storing the split-sum's second factor —
  the BRDF integral with F0 factored out as `F0*scale + bias` — precomputed
  once at startup via the same GGX importance sampling, using the IBL
  variant of the Smith geometry term (`k = roughness²/2`, distinct from the
  `k = (roughness+1)²/8` the direct-lighting BRDF uses — matching Smith's
  term to the importance-sampling PDF requires the different k).
- **Runtime cost**: two texture samples per pixel (`SampleLevel` on the
  prefiltered cubemap at `roughness * 4`, letting hardware trilinear
  filtering interpolate between the 5 precomputed mips for in-between
  roughness values, plus a `BRDFLUT` lookup) instead of a per-pixel
  importance-sampling loop — all the actual integration work happens once,
  CPU-side, at startup.

**Verified visually and by contrast**: a smooth metal sphere
(roughness≈0.05) shows a crisp, high-contrast mirror of the procedural bump
map's hex-cell pattern; the equivalent fully-rough metal sphere
(roughness=1.0) shows that same pattern completely blurred away, replaced by
soft, low-contrast colored blooms from the four point lights — the
qualitative signature of roughness-correct specular IBL, not achievable by
the old fade-strength-only stand-in (which kept the sharp pattern at every
roughness, just dimmer). 75/75 tests pass (unchanged — this follow-up has no
new unit-testable pure logic, only GPU pipeline/shader changes), plus 6 quick
runs and one 2000-frame stability run with no crashes.

### Follow-up: shadow mapping

`pbr_demo` gained a real depth-pass-then-sample shadow, cast by
`g_Lights[0]` onto a new ground plane added specifically to receive it (the
sphere grid alone has almost nothing for a shadow to visibly fall on —
neighboring spheres barely occlude each other at this grid's spacing).

- **Shadow pass**: a second, depth-only pipeline (`CreateShadowRootSignature`/
  `CreateShadowPSO`, minimal HLSL — one matrix in, `SV_POSITION` out, no
  pixel shader at all) renders every sphere (and only spheres — the ground
  plane is a receiver, not a caster) into a 1024×1024 depth buffer from
  `g_Lights[0]`'s point of view, recorded with raw D3D12 calls directly on
  the frame's command list rather than through `RenderGraph`: the shadow map
  is read (SRV) in the very same frame it's written (DSV) by a different
  pipeline, a multi-pass cross-usage pattern this file's `RenderGraph` usage
  wasn't set up to track. The resource itself is `DXGI_FORMAT_R32_TYPELESS`
  with separate `D32_FLOAT` (DSV) and `R32_FLOAT` (SRV) views — the standard
  way to get a depth buffer that's also sampled as a regular texture.
- **Sampling**: `ComputeShadow` (HLSL) projects the fragment's world
  position into the light's clip space and does 3×3 PCF (percentage-closer
  filtering) — 9 hardware comparison samples (`SampleCmpLevelZero` against a
  `SamplerComparisonState`, `D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT`)
  averaged into a soft 0..1 factor, with a small constant depth bias against
  shadow acne. Only `g_Lights[0]`'s contribution is multiplied by the
  factor — a point light's shadow genuinely needs a full cubemap (6 faces)
  to cover every direction it illuminates from, which is real future work;
  this follow-up proves the depth-pass/sample-back mechanics with one 2D
  shadow map instead.
- **Verified two ways, not just "didn't crash"**: the raw shadow map was
  dumped to a grayscale PNG (`AETHER_PBR_DEMO_SHADOW_MAP_SCREENSHOT`,
  `SaveShadowMapScreenshot`) and shows exactly the expected
  perspective-correct silhouette of all 49 spheres from the light's point of
  view. More importantly, the *sampled* shadow factor was visualized
  directly (temporarily returning `float4(shadowFactor.xxx, 1)` from the
  pixel shader instead of the lit color) and showed crisp, correctly
  perspective-shaped shadow ellipses on the ground plane, precisely aligned
  with each sphere — confirming the full pass end-to-end (generation,
  projection, PCF, and compositing), not just that the depth pass alone
  produces plausible-looking output. In the final lit composite the effect
  is present but visually subtle against this scene's strong ambient IBL
  and the ground's own bump-mapped normal texture — a lighting-balance/
  exposure detail, not a correctness one; the shadow-factor visualization is
  what actually demonstrates correctness. 81/81 tests pass (unchanged — pure
  GPU pipeline/shader work); 6 quick runs plus one 2000-frame stability run.

#### Follow-up: multi-light shadows

The single-light version above now casts shadows from `g_Lights[0]` **and**
`g_Lights[1]` — two independent shadow maps, not a shared/combined one.
Everything doubled up: `kNumShadowCasters = 2` shadow map resources/DSVs/SRVs
(`t4`, `t5`), `FrameConstants::light_view_proj` became a
`float4x4[kNumShadowCasters]` array, and the per-frame shadow pass now loops
over both casters, transitioning and rendering into each one's own depth
buffer in turn. `g_ShadowMaps[kNumShadowCasters]` (a real HLSL resource
array, not a bindless index) holds both maps; `ComputeShadow(casterIndex,
worldPos)` takes which caster to project through and sample.

- **A real fxc quirk found by actually compiling, not by inspection**:
  writing the per-light shadow selection as `(i < kNumShadowCasters) ?
  ComputeShadow(i, worldPos) : 1.0` inside the already-`[unroll]`ed 4-light
  loop failed to compile — `error X3504: literal loop terminated early due
  to out of bounds array access`. fxc, unrolling the outer loop, still
  elaborated `ComputeShadow(i, ...)`'s `g_ShadowMaps[i]` access for `i=2,3`
  even though that ternary branch is dead there for a 2-element array —
  apparently the ternary's false branch doesn't get proven unreachable
  before bounds-checking runs. Rewritten as explicit `if (i == 0) ... else
  if (i == 1) ...` branches (literal-compared against `i`, not a symbolic
  `kNumShadowCasters` bound) let the compiler actually eliminate the
  out-of-range calls instead of merely skipping them at runtime.
- Two shadow-casting point lights (as opposed to a single one) is a
  meaningfully different scene than uniformly extending to all four would
  be: the two chosen (`g_Lights[0]`/`[1]`, the "front" pair in this scene's
  corner layout) are the ones whose shadows actually fall across the visible
  ground plane from the demo's default camera framing. All four remaining
  point-light shadows (not just two) and true omnidirectional point-light
  shadows (a 6-face cubemap per light, not a single-direction perspective
  map) are still real future work.

**Verified visually**: both raw shadow maps were dumped
(`AETHER_PBR_DEMO_SHADOW_MAP_SCREENSHOT` now writes one file per caster,
`<path>` and `<path>_1`) and show the same 49-sphere grid from two
genuinely different (mirrored, since the lights sit at opposite grid
corners) points of view — not the same map duplicated. The two shadow
factors were then visualized simultaneously (`float4(shadow0, shadow1, 1,
1)` — cyan where only caster 0's shadow lands, magenta where only caster
1's does, navy where both overlap, white where neither does) and showed
three distinctly shaped, independently-positioned shadow regions on the
ground plane — confirming both maps are sampled and combined correctly,
not just that a second copy of the first one renders. 81/81 tests pass
(unchanged); 6 quick runs plus one 2000-frame stability run.

### Follow-up: glTF mesh loading (`aether::assets::LoadGltf`)

`engine/include/aether/assets/gltf_loader.h` loads real glTF 2.0 assets: JSON
parsed via `nlohmann::json` (`FetchContent`-fetched — a real, well-tested
parser rather than a hand-rolled one just for this), vertex data resolved
from either an external `.bin` file (relative to the `.gltf` path) or an
embedded `data:` base64 URI (own base64 decoder, ~30 lines). Scope:
POSITION/NORMAL/TEXCOORD_0 attributes, indexed triangle-mode primitives,
`pbrMetallicRoughness` materials with file-URI textures — a direct match for
the engine's own PBR metallic-roughness workflow (`pbr_demo`), so no
material-model conversion is needed — and the node hierarchy/transform tree
(see below). Not handled: skinning/animation, embedded (data-URI) images, and
non-metallic-roughness material extensions.

Verified two ways: unit tests (`tests/test_gltf_loader.cpp`) against a
hand-built triangle asset with an embedded base64 buffer (exact
positions/normals/UVs/indices, plus a materials-defaulted-per-spec case and
malformed-JSON/missing-file failure cases), and a genuine end-to-end render
(`gltf_demo/`) of `assets/models/test_textured_cube.gltf` — a real external
`.gltf` + `.bin` file pair, the code path the embedded-base64 unit tests
don't exercise. `gltf_demo` builds real GPU vertex/index buffers straight
from the loaded data; see the material system section below for how its
texture gets from the glTF file onto the GPU.

While testing this, caught a transcription error in the unit test's own
hand-typed base64 payload (a duplicated segment, giving a 114-byte buffer
instead of the intended 102) by actually running the test and getting a
specific, localized `CHECK failed: prim.indices[1] == 1` rather than
silently trusting the payload was correct — regenerated it programmatically
instead of re-editing it by hand.

#### Follow-up: node hierarchy / multi-node scenes

`LoadGltf` now walks the glTF `nodes`/`scenes` tree instead of treating every
primitive as if it were the scene's only object. Each node's local transform
comes from either a `matrix` (16 floats, column-major — matches
`aether::Mat4`'s own layout exactly, so it's a straight `memcpy`) or TRS
fields (`translation`/`rotation`/`scale`, each defaulting per spec when
absent; local = T·R·S). The tree is walked iteratively (explicit stack, not
recursion, to avoid stack-overflow risk on deep hierarchies), composing
`world = parent_world * local` — parent-then-child order, which matters
because these compositions don't commute — and flattened into
`GltfScene::node_instances`: a flat list of `{mesh_index, world_transform}`
pairs. A renderer just iterates that list and draws each mesh with its
resolved world transform; it never needs to walk the tree itself. A glTF file
with no `scenes`/`scene` array at all (rare, but technically spec-valid)
falls back to treating every node that isn't referenced as another node's
child as an implicit root.

Verified with 3 new unit tests (`tests/test_gltf_loader.cpp`): one uses a
non-commuting scale-then-translate pair specifically so an accidentally
reversed multiplication order (`child * parent` instead of
`parent * child`) would produce a different, wrong world position and get
caught — pure translations would happen to pass either way, so they wouldn't
actually test the order. The other two cover multiple root nodes sharing one
mesh, and the no-`scenes`-array root-detection fallback. 75/75 tests overall.

`gltf_demo` renders `assets/models/test_scene.gltf`, a hand-built 4-node
hierarchy (a root translated node with two children, one of which itself has
a child) producing 3 cube instances at different, hierarchy-derived world
positions — confirmed by an actual screenshot showing three checkered cubes
at distinct positions, one visibly offset from and near its parent rather
than at the origin, not just asserted from the code. Verified stable across
6 quick runs plus one 2000-frame run.

#### Follow-up: skinning + animation data (`GltfSkin`/`GltfAnimation`)

`LoadGltf` now also parses `skins` (joints + inverse bind matrices, defaulting
to identity inverse binds per spec when the accessor is omitted) and
`animations` (translation/rotation/scale channels, LINEAR/STEP
interpolation — CUBICSPLINE channels are skipped, same treatment as an
unsupported primitive mode elsewhere in this loader) into
`GltfScene::skins`/`GltfScene::animations`. The node hierarchy is now also
kept unflattened (`GltfScene::nodes`/`root_nodes`, TRS fields kept separate
rather than pre-baked into a matrix) specifically so playback can override
individual components per channel. `GltfPrimitive` gained parallel
`joint_indices`/`joint_weights` arrays, populated only when a primitive has
both `JOINTS_0` and `WEIGHTS_0` accessors.

Two runtime entry points do the actual playback math:

- `EvaluateAnimation(scene, animation, time_seconds, out_node_instances)` —
  re-walks the hierarchy with a channel-animated copy of the nodes (the
  original `GltfScene::nodes` is never mutated), producing the same
  flattened `{mesh_index, world_transform}` shape the static bind pose
  already exposes. Time is clamped to `[0, duration]`, not looped — a caller
  wanting looping playback sends `fmod(t, duration)` itself.
- `ComputeSkinMatrices(scene, animation, time_seconds, skin, out_matrices)` —
  `joint_world_transform * inverse_bind_matrix` per joint, ready for a
  future GPU skinning shader to consume. Quaternion rotation channels use
  component-wise LERP + renormalize rather than SLERP (a standard cheap
  approximation, accurate enough for reasonably dense keyframes).

Verified with 6 new unit tests (`tests/test_gltf_loader.cpp`): channel
times/values decode correctly, `EvaluateAnimation` interpolates linearly
mid-segment *and* clamps rather than extrapolates past the last keyframe,
a skin's matrices reduce to identity at rest pose with a genuinely
non-identity inverse bind matrix (not the degenerate identity/identity case,
which would pass even with the joint-world/inverse-bind multiplication
order reversed), the identity-inverse-bind default, and
`JOINTS_0`/`WEIGHTS_0` parsing. 81/81 tests overall.

`gltf_demo`'s `AETHER_GLTF_DEMO_ANIMATE=1` mode is the end-to-end render
proof for the animation half: it loads `assets/models/test_animation.gltf`
(a single cube whose node slides from x=-2 to x=+2 and back, LINEAR,
duration 2s) and calls `EvaluateAnimation` every frame — confirmed by two
screenshots at different frame counts showing the cube at visibly different
on-screen positions, not just logged as loaded. Stable across 6 quick runs
plus one 2000-frame run.

#### Follow-up: GPU vertex skinning

The one gap the animation follow-up above left explicitly open — real GPU
skinning consuming `joint_indices`/`joint_weights`/`ComputeSkinMatrices`
rather than just parsing and unit-testing them — is now closed.
`AETHER_GLTF_DEMO_SKIN=1` runs a deliberately separate demo/pipeline (its
own shader, vertex layout, and root signature — no bindless textures or
material system, since the point is proving the skinning math, not
re-proving texture sampling already covered elsewhere): it loads
`assets/models/test_skinned_ribbon.gltf`, a hand-built 12-vertex, 2-joint
ribbon (joint 0 fixed at the base, joint 1 at the midpoint, per-row
`WEIGHTS_0` blending smoothly from 100% joint 0 at the base to 100% joint 1
at the tip) whose single animation channel swings joint 1's rotation back
and forth ±30° over 2 seconds.

Every frame, `ComputeSkinMatrices` (CPU-side, same function the animation
follow-up already unit-tested) computes the current joint matrices and
uploads them to a small root-CBV constant buffer (8 `float4x4` slots — well
past the 64-DWORD limit a single root-32-bit-constants parameter allows,
which is why this needs an actual CBV rather than inline constants like the
rest of this demo's root signature). The vertex shader does the actual
skinning: standard linear blend skinning, `skinMat = Σ g_Joints[joint_i] *
weight_i`, then `skinnedPos = mul(skinMat, position)` before the usual
model/view/projection transform — computed per-vertex, per-frame, entirely
on the GPU.

**Verified visually**: two screenshots at different points in the animation
cycle show the ribbon bent in visibly different amounts/directions (not a
static pose) with a smooth, continuous curve from base to tip — the
qualitative signature of correct per-vertex weight blending, since a bug in
the blend math (e.g. hard-switching between joints instead of interpolating)
would show up as a sharp kink at a fixed row instead. 81/81 tests unchanged
(pure GPU pipeline/shader work, using already-unit-tested CPU-side math);
existing gltf_demo modes (test_scene.gltf, `AETHER_GLTF_DEMO_ANIMATE=1`)
regression-checked; 6 quick runs plus one 2000-frame stability run.

### Follow-up: material system (`aether::gfx::MaterialData`/`LoadMaterial`)

`engine/include/aether/gfx/material.h` is the bridge between a loaded glTF
material description and something a shader can actually use: `MaterialData`
is a GPU-ready struct (base color factor, metallic, roughness, three
bindless texture indices) uploadable as-is via root 32-bit constants, and
`LoadMaterial` resolves a `GltfMaterial`'s texture paths through
`AssetManager` — so a texture referenced by two different materials (or
already loaded for another purpose) is decoded/uploaded at most once,
`AssetManager`'s caching doing exactly its job across an actual multi-user
scenario, not just within one material. A texture field is
`DescriptorHeap::kInvalidIndex` when the material has none; shaders check
for that and fall back to the factor alone, matching the glTF spec's own
factor-times-texture-or-factor-alone semantics.

`gltf_demo` is the end-to-end proof: it loads
`assets/models/test_textured_cube.gltf` (referencing
`assets/textures/checker_a.png`, already used by the asset pipeline's own
example — see that section), resolves its material via `LoadMaterial`, binds
a bindless descriptor heap and the resulting texture index, and *samples
that texture in the pixel shader* — not just carries the index around
unused. The rendered cube visibly shows the checkerboard pattern mapped
correctly across all six faces, confirmed visually, not assumed from the
index being non-`kInvalidIndex`.

Verified via 3 new unit tests (`tests/test_material.cpp`: factor copying +
texture resolution, cross-material cache sharing, and the
no-texture-leaves-indices-invalid case) — 72/72 tests overall — plus repeated
`gltf_demo` runs (6x quick + one 2000-frame run).

### Follow-up: editor glTF integration

Every glTF-related follow-up above was proven out in a standalone demo
(`gltf_demo`); this one connects that pipeline to the actual gameplay
editor (`editor/`, Phase 5's ImGui + ECS + Jolt physics overlay) instead —
loading a real, textured, animated glTF asset into the same scene the
ECS/physics entities already live in, not another separate program.

- **A second, independent render pipeline** (`CreateGltfRootSignature`/
  `CreateGltfPSO`, the same shader/vertex-layout/bindless-material shape
  `gltf_demo` uses) draws `assets/models/test_animation.gltf` — a real
  `LoadGltf` load, a real `LoadMaterial`-resolved bindless texture, a real
  `EvaluateAnimation` call every frame — alongside the editor's existing
  billboard-quad ECS entity renderer, in the same render pass.
- **A real depth buffer**, added specifically for this follow-up: the
  editor's billboard spheres never needed one before (drawn with
  `DepthEnable = FALSE`, correct by construction since they're flat,
  camera-facing quads with no real depth relationships to get wrong). A
  genuine 3D mesh sharing the scene changes that — both pipelines now
  depth-test against one shared, `RenderGraph`-managed depth buffer
  (recreated on resize, same pattern `gltf_demo`/`pbr_demo` already use), so
  the glTF model and the physics spheres occlude each other correctly by
  actual depth rather than by draw order.
- **A live "glTF Model" ImGui panel**: source path, index count, resolved
  material base color, and — when the asset has an animation, which this
  one does — an "Animate" checkbox and a running `current/duration` time
  readout, wired straight to the same `EvaluateAnimation` call driving the
  render.
- **A real bug caught by actually looking at the screenshot, not by
  inspection**: the model's first placement (offset to the side of the
  falling-sphere playground, chosen by eyeballing world-space coordinates)
  rendered nothing at all — not a black cube, no draw call trace, just
  absent. It was simply outside the camera's frustum; `LookAtRH`'s
  particular handedness/right-vector convention here also means +X world
  maps to the *left* of the screen for this camera setup, not the right, as
  discovered by testing at the origin (visible, centered) and then at a
  positive-X offset (moved *toward* the ImGui panel on the left instead of
  away from it) — not something that would have been caught by reading the
  matrix math alone.

**Verified visually across two frames**: the model appears as a distinctly
checkered cube next to (not overlapping) the ECS-driven spheres, with the
ImGui panel's displayed animation time genuinely advancing between
screenshots (confirmed at two frame counts deliberately *not* an exact
multiple of the 2-second animation's period apart, after an initial pair of
screenshots happened to land on the same phase by coincidence and looked
identical) and the cube visibly at a different position each time. A
physics sphere is also visibly correctly occluded by/occluding the glTF
mesh in one of the two screenshots — confirming the shared depth buffer
composites the two pipelines correctly, not just that both draw calls
execute without crashing. 81/81 tests unchanged (pure integration/rendering
work, no new pure logic); 6 quick runs plus one 2000-frame stability run.

### Follow-up: toward a usable editor workflow

The glTF integration above was still a single hardcoded model bolted onto
the side of the editor — always present, one fixed path baked into source,
rendered outside the ECS entirely. This follow-up makes a model a normal
part of the same entity workflow the physics spheres already have: spawn
it, see it in the entity list, move it, select it, save it, delete it.

- **Models are entities now.** `ModelRenderer` (`{ char asset_path[128]; }`)
  is a component like any other — `world.CreateEntity(Transform{...},
  ModelRenderer{...})` — not a `gltf_scene`/`gltf_loaded` pair of local
  variables living outside the ECS. It's plain fixed-size data rather than
  `std::string` specifically so it needs **no custom serializer** to
  round-trip through Save/Load Scene: the default raw-byte `ComponentInfo`
  serializer every component type gets for free (the same one `Transform`
  already relies on) is already correct for it. The actual GPU/CPU
  resources (vertex/index buffers, resolved material, parsed `GltfScene`)
  live in `GltfCache`, a `path -> GltfRenderData` map — a component can't
  own a GPU buffer or a variable-size string in the ECS's fixed-slot chunked
  storage, so the component only carries the *reference*, and every entity
  pointing at the same asset path shares one `GltfRenderData` (uploaded
  once), the same caching discipline `AssetManager` already applies to
  individual textures.
- **In-editor asset picker.** `ListAvailableGltfModels()` scans
  `assets/models/*.gltf` at startup; the "Add Model" panel lists whatever
  it finds as buttons — click one to spawn a new entity referencing it, no
  path hardcoded in source.
- **Scene save/load round-trips models.** Verified directly, not assumed
  from the component being POD: a temporary self-test (`SaveScene` a world
  with a `ModelRenderer` entity, `LoadScene` into a fresh `World`, confirm
  the loaded entity's `asset_path` matches) logged `PASS` before being
  removed — the mechanism is also covered generically by
  `tests/test_serialization.cpp`'s existing round-trip test, which already
  exercises an arbitrary plain-data component the same shape `ModelRenderer`
  is.
- **Selection + Inspector**, the scoped form: a "Select" button per
  entity-list row (bodies and models both) sets an `Entity selected_entity`,
  which opens a dedicated Inspector panel for *only* that entity and tints
  its rendered color/material toward gold — real, visible selection
  feedback. What this deliberately isn't yet: clicking directly on an
  object *in the viewport* to select it, which needs screen-to-world ray
  casting against each entity's bounds, and a draggable 3D transform gizmo
  instead of raw `DragFloat3` fields — both real future work, not silently
  dropped scope.
- `ForEachWithEntity<Components...>` — built on the same type-erased
  `Archetype`/`EntityArray()` access the scene serializer already uses — is
  what makes any of this possible: `World::ForEach` hands back component
  references but not the `Entity` they belong to, and an editor that needs
  to select or delete *this specific entity* needs the handle, not just its
  data.

**Verified visually and by an actual round-trip, not by inspection**: the
Add Model panel correctly lists all four `.gltf` files under
`assets/models/`; selecting the initial model shows its live Inspector data
(source path, material, Animate toggle, running time) *and* renders it with
the gold highlight tint, confirmed in a screenshot; the Save/Load self-test
logged a genuine `PASS`. 81/81 tests unchanged (ECS/editor integration
work, not new pure logic needing new unit tests); 6 quick runs plus one
2000-frame stability run.

### Follow-up: viewport picking, transform gizmo, scene hierarchy, asset browser, editor camera

The previous follow-up explicitly deferred real click-in-viewport picking,
a draggable gizmo, and any parent/child concept — this one builds all of
that, plus the fly camera picking/gizmos need to be meaningful against.

- **Editor camera (`EditorCamera`).** Replaces the hardcoded static
  `Mat4::LookAtRH(Vec3(0,6,-14), Vec3(0,1,0), Vec3(0,1,0))` with a real
  fly camera: hold Right Mouse to look around (mouse delta → yaw/pitch) and
  move with WASD/QE while it's held, gated on `!ImGui::GetIO().WantCaptureMouse`
  so dragging an ImGui panel or slider never also spins the camera —
  standard Unity/Unreal-style scheme, needs no in-app explanation.
- **Viewport picking.** A mouse click unprojects into a world-space ray
  (`ScreenPointToRay`, via a general 4x4 matrix inverse of `view_proj` —
  `InverseGeneral`/`InvertMatrix4x4`, since `Mat4` had no inverse of its
  own), tested against every `RigidBody`'s sphere and every
  `ModelRenderer`'s bounding sphere (`RaySphereIntersect`). The model
  bounding sphere is computed once at load time from the raw vertex extents
  (`GltfRenderData::bounds_center`/`bounds_radius`) — approximate, not
  per-triangle, but correct enough to click a model. Clicking empty space
  deselects, matching every mainstream 3D editor. This runs alongside (not
  instead of) the existing list-based "Select" buttons.
- **Transform gizmo.** A minimal unlit line-list pipeline (its own PSO/root
  signature — `CreateGizmoPSO`/`CreateGizmoRootSignature`, depth-test off so
  it's always visible on top, same as every mainstream editor's gizmo) draws
  three axis-colored lines at the selected entity's position. Dragging an
  axis moves the entity strictly along that one world-space axis regardless
  of camera angle, via the standard closest-point-between-two-lines
  formula (`ClosestPointOnAxisToRay`) rather than a naive screen-space-only
  projection — the same technique real 3D-editor gizmos use. A `RigidBody`
  being dragged re-teleports its physics body the same way the Inspector's
  own `DragFloat3` position edit already does.
- **Scene hierarchy.** A new `Parent { Entity entity; }` component plus
  `ComputeWorldTransform` (walks the `Parent` chain, composing local
  transforms into a world one, bounded to 32 steps against a cycle) give the
  previously-flat ECS a real parent/child concept for the first time. The
  Hierarchy panel renders it as an actual `ImGui::TreeNodeEx` tree, with a
  "Parent to selection" button per row (rejected via `WouldCreateCycle` if
  it would make an entity its own ancestor) and "Unparent" to detach.
  Deleting a parented-to entity unparents its children first (collected,
  then applied — mutating an entity's archetype from inside the same
  `ForEachWithEntity` iteration that found it would invalidate that
  iteration mid-flight) so no `Parent` is ever left dangling at a
  stale/reused entity index.
- **Asset browser.** What was an inline "Add Model" button list inside the
  main panel is now its own "Asset Browser" window, with a **Refresh**
  button that re-runs `ListAvailableGltfModels()` — a `.gltf` dropped into
  `assets/models/` while the editor is running now shows up without a
  restart.

**Verified by actual execution, not by code review alone**: a temporary
self-test (`AETHER_EDITOR_SELFTEST_HIERARCHY`) parented one model to
another, moved the parent by a known delta, and asserted the child's world
position moved by that same delta while its local `Transform` stayed
untouched — logged a genuine `PASS`. A second temporary self-test
(`AETHER_EDITOR_SELFTEST_GIZMO`) forced a known selection and camera
framing and confirmed the axis-colored gizmo lines actually render (visible
in a screenshot) at the same time as the Inspector panel. Both self-tests
were removed before commit, same discipline as the earlier Save/Load
round-trip self-test. A missing
`D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` flag on the
gizmo's root signature caused an immediate `CreateGraphicsPipelineState`
failure (`E_INVALIDARG`) the first time it ran — fixed by copying the flag
the glTF root signature already sets for the same reason (its PSO also has
a real vertex input layout, unlike the billboard-sphere pipeline). 81/81
tests unchanged; 6 quick runs plus one 2000-frame stability run, all clean.

### Follow-up: friendlier UI (layout, style, confirmations, help)

The previous two follow-ups added real functionality (camera, picking,
gizmo, hierarchy, asset browser) but left the UI itself exactly as
default-ImGui-gray as it always was, with every panel spawning at the same
default position — the screenshots showed Hierarchy, Inspector, and the
main panel all stacked directly on top of each other. This follow-up is a
pure UI/UX pass, no new editor functionality.

- **Non-overlapping default layout.** Every panel (`Aether Editor`, `Asset
  Browser`, `Hierarchy`, `Inspector`) now gets an explicit default
  position/size via `ImGui::SetNextWindowPos`/`SetNextWindowSize` with
  `ImGuiCond_FirstUseEver` — a simple left-column stack (main panel → Asset
  Browser → Hierarchy) plus Inspector on the right, leaving the 3D viewport
  visible in between. `ImGuiCond_FirstUseEver` means this only applies
  before a window has a saved position in `imgui.ini`; once a user drags a
  panel themselves, their own layout persists across runs. (A real docked
  layout via `ImGui::DockBuilder`/`DockSpace` was the first approach tried,
  but this project's vendored ImGui is pinned to a plain release tag
  (`v1.92.9`) rather than the separate `docking` git branch, so those
  symbols don't exist in this build — reverted in favor of the
  `SetNextWindowPos` approach above rather than repointing the whole
  project at a different ImGui branch for one follow-up.)
- **Friendlier visual style** (`ApplyFriendlyEditorStyle`): a soft blue-teal
  accent color (buttons, headers, checkmarks, tabs) in place of ImGui's
  default blue, rounded corners, and more breathing room between widgets.
  `ImGui::SeparatorText` section headers ("Controls", "Bodies", "Models")
  replace the old plain `Text` + `Separator` pairs. Bodies/models are
  color-coded by type and selection state (blue for a body, orange for a
  model, gold when selected) instead of a bare `#N`.
- **Friendlier controls**: field labels now carry units ("Position (m)",
  "Radius (m)", "Mass (kg)", "Static (doesn't fall)") instead of bare nouns.
  Every **Delete** button (Bodies, Models, and the Hierarchy panel) now
  opens a shared "Are you sure?" confirmation modal instead of deleting
  immediately and irreversibly on a single misclick.
- **Onboarding**: a new **Help** window, shown by default on first run,
  centered on screen, with a plain-language quick reference for the camera
  controls, selecting/moving, and the scene hierarchy — the one thing the
  camera/picking/gizmo follow-up left completely undocumented in-app. A
  **View** menu (new main menu bar) can re-show it, or any panel closed via
  its title-bar X, without restarting.

**Verified by actual execution**: screenshots (with `imgui.ini` cleared to
force first-run defaults) confirm all five panels lay out without
overlapping, the accent color/section headers/unit labels render correctly,
and — with a temporary env-var-gated self-test forcing a selection, removed
before commit — that the Inspector's default top-right position doesn't
collide with the left-column stack even when populated with real model
data. 81/81 tests unchanged; 6 quick runs plus one 2000-frame stability
run, all clean.

### Phase 6 — Reflection & Property System

First step of the [roadmap](docs/ROADMAP.md)'s Phase 6 (build spec:
[`docs/ROADMAP_DETAILS.md` §B](docs/ROADMAP_DETAILS.md)).
`engine/include/aether/reflection/` lets the engine describe its own types at
runtime, which the generic Inspector, versioned serialization, undo/redo,
scripting and Blueprints all build on.

- **`TypeInfo` / `FieldInfo`**: name, kind (bool/int/uint/float/string/enum/
  struct), size, alignment, schema version, fields in declaration order
  (offset, field type, `FieldFlags`, editor `Meta` such as tooltip, category,
  range and units), enum name/value tables, and construct/destruct/
  copy/move thunks. `FieldInfo::As<T>(obj)` gives typed access and returns
  `nullptr` on a type mismatch instead of reinterpreting memory.
- **`TypeId`**: FNV-1a 64 of the *declared* name (`"Health"`, namespace
  stripped). This replaces `typeid(T).name()`, whose output differs between
  compilers, as the future on-disk key. The hash is pinned by a test.
- **`TypeRegistry`**: find by name or `TypeId`, list all types. Types
  register statically, so `Find("Health")` works before anything calls
  `Reflect<Health>()`. A second type with the same declared name is rejected
  with an error log.
- **Macros**: `AETHER_REFLECT(Type, version, AETHER_FIELD(member, flags,
  {.meta...}), ...)` and `AETHER_ENUM(Type, version, AETHER_ENUM_VALUE(...))`,
  used at global scope. A field of an unreflected type is a compile error
  with a message saying what to add.
- **Builtins**: `bool`, `i8`–`i64`, `u8`–`u64`, `f32`, `f64`,
  `std::string`, `Vec3`, `Vec4`, `Quaternion` and `Mat4` (as four `Vec4`
  columns).

**Step 2: `Any` and function reflection.**

- **`Any`** (`any.h`): holds one value of any reflected type together with
  its `TypeInfo`. Values up to 32 bytes are stored inline; larger ones, such
  as `Mat4`, go on the heap with the correct alignment. Copying an `Any`
  copies its value through the type's own copy constructor, and moving one
  leaves the source empty. `TryGet<T>()` returns `nullptr` on a type
  mismatch.
- **Field access through `Any`**: `FieldInfo::Get(obj)` returns a copy, and
  `FieldInfo::Set(obj, any)` refuses a value of the wrong type and leaves the
  field unchanged.
- **Functions**: `AETHER_METHOD(name, flags, {"param", "names"})` goes in the
  same `AETHER_REFLECT` list as fields. It records the return and parameter
  types, marks `const` and `static` functions automatically, and generates a
  call thunk. `FunctionInfo::Invoke(self, args, &ret)` checks the argument
  count, every argument type, and that member functions get an instance, and
  calls nothing if any check fails. A parameter taken as `T&` writes its
  result back into its `Any` argument.

**Step 3: JSON and binary archives** (`serialize.h`).

- `SaveJsonText`/`LoadJsonText` and `SaveBinary`/`LoadBinary` work for any
  reflected value, with no per-type code.
- Both archives share one document model. Binary is the same document
  encoded as MessagePack, so it's smaller but keeps field names and is just
  as tolerant of schema changes. The compact cooked format with no field
  names comes later, with the cooker.
- **Readable, deterministic JSON**:
  - Keys are sorted, and each struct records its schema version as `"$v"`.
  - Enums are written by name, and math types as arrays (`[x, y, z]`).
  - `f32` values use their shortest exact form (`0.1`, not
    `0.10000000149011612`).
  - Saving the same object twice gives byte-identical text, and so does
    save → load → save.
- **Tolerant loading**:
  - A field missing from the data keeps its default.
  - Data for a field that no longer exists is ignored.
  - Wrong-typed values, out-of-range integers and unknown enum names are
    skipped with a warning that names the path (`Everything.mood`), and the
    rest of the object still loads.
  - Data saved by a newer version loads the fields this version knows, with
    a warning.
- **Schema migration**: `RegisterMigration<T>(fn)` edits old data (for
  example renaming `hp` to `health`) before it's loaded, in both archive
  formats.

**Step 4: ECS and scene integration.**

- **Reflected components** get reflection-based defaults in
  `GetComponentId<T>()`. They're named by their declared name (`"Stamina"`)
  instead of the compiler-specific `typeid` name, and saved field by field
  through the binary archive. Their old `typeid` name is also registered as
  an alias, so older scene files still find them. Components that aren't
  reflected, and custom serializers such as `RigidBody`'s, behave exactly as
  before.
- **More component types:** the cap goes from 64 to **256**. Archetypes are
  now looked up by the full `ComponentMask` rather than `mask.to_ullong()`,
  which only worked with 64 or fewer.
- **Scene format v2 (`.aesc`):**
  - Each entity stores its component count instead of a 64-bit mask.
  - Each component stores an encoding byte (raw, custom or reflected).
  - Entities are written in creation order, so saving the same world twice,
    or load → save, gives identical bytes.
  - **v1 files still load**, including raw-byte components into types that
    have since become reflected (when the type is plain data and its size is
    unchanged).
- **JSON scenes (`.ascene`):** `SaveSceneJson`/`LoadSceneJson` use the
  §A.3 layout. Only reflected components are written, and anything else is
  left out with a warning. Unknown components and fields are skipped when
  loading.
- **Build fix:** Jolt is now built with C++ RTTI. `JoltJobSystemAdapter`
  derives from a Jolt class in our RTTI-enabled code, and GCC and Clang
  failed to link without it. Physics builds and tests on Linux for the first
  time.

**Step 5: the existing components are reflected.**

- **`Transform`** (position in m, rotation) and **`RigidBody`** (radius,
  mass, `is_static`, with editor ranges and units) are reflected in
  `physics/components.h`. `RigidBody`'s live Jolt `body_id` is left out on
  purpose, and its custom binary serializer is kept, so existing `.aesc`
  files load unchanged. Both components now round-trip through JSON scenes.
- **`ModelRenderer`** moved from `editor/main.cpp` to
  `engine/include/aether/scene/components.h` and is reflected there, since
  it's a runtime component. The move follows the plan in
  `docs/design/EDITOR_UI.md` §10.
  - Its old in-editor type names are registered as aliases through the new
    `RegisterComponentAlias<T>()`, so editor scenes saved before the move
    still find their models.
  - The GCC/Clang alias string was checked against those compilers' actual
    `typeid` output. The MSVC string follows MSVC's documented format but
    hasn't been run.
- **Fixed-size strings:** reflection now supports `char[N]` fields
  (`TypeKind::FixedString`). They're saved as strings, and a string that's
  too long is truncated with a warning when loading.
- **Not reflected:**
  - `Parent` holds a live `Entity` handle, which can't be remapped on load
    until Phase 7's entity GUIDs. It keeps its raw-byte behavior for now.
  - `EditorCamera` is a local in the editor, not an ECS component, so it
    needs no reflection.

**Step 6: the generic Inspector.** With this step Phase 6 is complete.

- **`editor/src/ui/reflected_inspector.{h,cpp}`** (new `aether_editor_ui`
  library): `InspectObject(type, object, id)` draws any reflected object as
  a two-column property table, following `docs/design/EDITOR_UI.md` §5.1.
  - Checkboxes for bools.
  - Drags for numbers, clamped to the field's `Meta` range and showing its
    units.
  - Text fields for `std::string` and `char[N]`, and dropdowns for enums.
  - One X/Y/Z(/W) row for small float structs (`Vec3`, `Vec4`,
    `Quaternion`), and collapsible sub-tables for other structs.
  - `Meta::category` headings and `Meta::tooltip` on hover.
  - It reports which top-level field changed, and when the edit was
    committed, so later phases can record one undo step per edit.
  - Only fields flagged `Field_EditAnywhere` (editable) or `Field_ReadOnly`
    (disabled) are shown. Nested fields take their parent's setting.
  - `AddComponentButton` is a searchable "+ Add Component" popup.
- **The library is portable.** It depends only on ImGui and reflection, so
  it builds and is tested **headless** on every platform. For that, ImGui is
  now split into `imgui_core` (portable, plus `imgui_stdlib`) and the
  Win32/DX12 backend library `imgui`. The editor executable is only
  configured on Windows, which its Win32 backend already required.
- **The editor's Inspector panel is now generic.** It draws every reflected
  component on the selection, with no per-component UI code.
  - The only component-specific code left makes edits reach the physics
    simulation: a `Transform` edit teleports the Jolt body, and a
    `RigidBody` edit recreates it, as the body list already did.
  - "+ Add Component" lists every reflected component the entity doesn't
    have yet. Adding `RigidBody` also adds a `Transform` and creates the Jolt
    body.
  - The glTF load status and animation controls, which aren't component
    data, are still drawn by hand below.
- **Type-erased `World` calls:** `World::AddComponentRaw`,
  `RemoveComponentRaw` and `HasComponentRaw`, plus
  `RegisteredComponentCount()`, for code that only knows a component's ID at
  runtime.

**Verified** by building the engine and tests on Linux with both GCC 13
and Clang, with no warnings from the new code. There are 29 reflection and serialization
tests (`tests/test_reflection.cpp`, `tests/test_reflection_any.cpp`,
`tests/test_archive.cpp`, and the Phase 6 section of
`tests/test_serialization.cpp`). All 87 tests pass with physics off, and
94/94 with physics (Jolt) enabled.

- Five of those tests drive a real, headless ImGui context with simulated
  keyboard input. They type into string, number and `char[N]` fields and
  check the edited values, Meta range clamping, and commit reporting.
- They also pass in the Debug build, where ImGui's internal assertions are
  active.
- The editor executable itself (Win32/D3D12) couldn't be built here. Its new
  Inspector block was compiled on its own, pasted word for word into a small
  file against the real engine, physics, ImGui and Inspector headers. The
  rest of the editor diff is two alias registrations, one include, and
  removing its local `ModelRenderer`. A Debug build with AddressSanitizer and
UndefinedBehaviorSanitizer also passes cleanly, including a test with an
instance-counting type showing that `Any` constructs and destroys each value
exactly once, both inline and on the heap. That run excludes the
Windows-only D3D12 tests; the MSVC build hasn't been run for this change.
The macros use `##__VA_ARGS__` (already used by the log macros) rather than
`__VA_OPT__`, which MSVC only supports with `/Zc:preprocessor`.

### Phase 7 (in progress) — Editor Foundation

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md), Phase 7.

**Step 1: permanent entity IDs** (`engine/include/aether/scene/entity_guid.h`).

- **`EntityGuid`**: a random RFC 9562 version 4 UUID, created once and saved
  with the entity. An `Entity` handle changes whenever an entity is
  destroyed and recreated (undo, reloading, prefabs), so saved data and undo
  history will refer to entities by GUID instead.
  - `NewEntityGuid()` is thread-safe, with one OS-seeded generator per
    thread.
  - `ToString`/`ParseEntityGuid` use the canonical
    `5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f77` form.
- **`IdComponent`**: gives an entity its GUID. It's reflected, and shown
  read-only in the Inspector. `EnsureGuid` adds one when missing, and
  `RegenerateGuid` gives a duplicate a fresh ID.
- **`GuidIndex`**: maps GUID → current `Entity`.
  - `Find` never returns a destroyed entity, and never returns whatever new
    entity has reused its slot.
  - `Rebuild(world)` reindexes after a scene load and reports duplicate GUIDs
    (the same scene loaded twice, for example). The earlier entity keeps the
    GUID.
- **Scene files**: JSON scenes write the ID at entity level as
  `"guid": "…"` (§A.3 layout) rather than among the components. Both formats
  round-trip it.
- **Custom JSON converters** (`reflection/converters.h`):
  `RegisterJsonConverter` lets a type pick its own JSON form (a string for
  GUIDs, rather than `{hi, lo}`), in both JSON and binary archives. This will
  also serve asset GUIDs in Phase 8.
- **`World::IsAlive(Entity)`**.

**Verified**: 6 new tests, including 20,000 generated GUIDs checked for
uniqueness and version/variant bits, parse rejection cases, stale-handle
rejection after destroy and slot reuse, duplicate detection, and
byte-identical JSON re-saves. 93/93 tests pass on GCC 13, on Clang and
under ASan/UBSan, and 100/100 with physics.

**Step 2: the undo/redo command stack** (`editor/src/core/command_stack.h`,
part of the portable editor library, so it's tested headless).

- `ICommand` has `Do`, `Undo`, `Label`, `TryMerge` and `MemoryBytes`.
  Commands refer to entities by `EntityGuid`, so undoing a delete (which
  recreates the entity with a new handle) keeps every later command working.
- **Merging**: `Execute(ctx, cmd, MergePolicy::Allow)` folds successive
  values from one slider or gizmo drag into a single undo step, until
  `BreakMergeChain()` is called when the edit is committed. Undo, redo,
  saving and transactions also break the chain, so a merge never reaches
  back past any of them.
- **Transactions**: `BeginTransaction`/`EndTransaction` group a multi-step
  action (duplicate, paste, create prefab) into one entry. Nested pairs are
  allowed, and undo/redo are refused while a transaction is open.
- **Unsaved-changes tracking**: `MarkSaved`/`IsDirty` know when undo or redo
  lands back on the saved state, and when that state has become unreachable
  (redo history replaced, or the entry dropped by the budget).
- **Memory budget** (256 MB by default): the oldest entries are dropped
  first, and the newest one is always kept.

**Verified**: 7 new tests with test-only create, destroy and set commands on
a real `World`. One is the spec's randomized test: 1,000 random creates,
destroys, sets, merged drags and transactions, then undo-all must reproduce
the original state exactly and redo-all the state after the run.

- The comparison is a GUID → content snapshot rather than raw scene bytes,
  because recreated entities get new handles and therefore a different
  storage order.
- 100/100 tests pass on GCC 13, on Clang and under ASan/UBSan, and 107/107
  with physics.

**Step 3: built-in commands, and Ctrl+Z in the editor.**

- **Commands** (`editor/src/core/commands.h`): `SetFieldCommand` (merges
  drags), `CreateEntityCommand`, `DestroyEntityCommand`,
  `AddComponentCommand` and `RemoveComponentCommand`. They work for any
  component through the ECS registry and reflection.
  - `EntitySnapshot` saves every component of an entity (reflected, raw or
    custom-serialized), so deleting and undoing restores the entity exactly,
    with the same GUID.
  - `CreateEntityCommand::FromExisting` plus the new `CommandStack::Record`
    capture something other code already spawned, without creating it twice.
  - `EnsureAllGuids` gives a loaded world unique IDs.
- **`EditorHooks`**: callbacks for created, destroying, component added or
  removing, and field changed. They fire on the first execution and on every
  undo and redo, so systems outside the ECS can follow along. The commands
  themselves know nothing about physics.
- **`InspectEntity`** (`editor/src/ui/entity_inspector.h`): the editor's
  Inspector body.
  - Every edit becomes a `SetFieldCommand`: the widget's in-place change is
    reverted and re-applied through the command. A drag is one undo step,
    ending when the edit is committed.
  - Adding and removing components (right-click a component's header) are
    commands too.
  - The entity's ID is shown as a line of text rather than as an editable
    card.
- **Editor**:
  - Ctrl+Z, and Ctrl+Y or Ctrl+Shift+Z (ignored while typing in a text
    field), plus an Edit menu showing the next Undo/Redo label.
  - Spawning a sphere or model and deleting are undoable. The hooks
    create and destroy Jolt bodies, teleport a body on a `Transform` edit and
    recreate it on a `RigidBody` edit, including on undo and redo.
  - Saving marks the history clean; loading a scene clears it and gives
    every entity an ID.
  - Stale handles (a selection, pending delete, or `Parent` link to an
    entity that undo destroyed) are dropped instead of followed. The
    hierarchy walks check `IsAlive`.
- Two gaps from this step were closed in step 4: `Parent` links surviving
  undo, and the body-list sliders going through commands.

**Verified**: 6 new tests, including a headless ImGui test that types a
value into the Inspector and checks it becomes one labelled, undoable
command. 106/106 tests pass on GCC 13, on Clang and under ASan/UBSan, and
113/113 with physics. The editor executable isn't built here (Win32/D3D12).
Every changed block of `editor/main.cpp` (hooks, shortcuts, Edit menu,
spawn/save/load, Inspector call, delete loop, stale-link cleanup) was
compiled together in a harness against the real headers, with no warnings.

**Step 4: the scene hierarchy, reparenting, and gizmo/slider undo.**

- **`engine/include/aether/scene/hierarchy.h`** (moved out of the editor):
  - `Parent` now stores the parent's **`EntityGuid`**, not an `Entity`
    handle, and is reflected.
  - `GetParent`, `ComputeWorldTransform`, `WorldPosition`,
    `WouldCreateCycle` and `ChildrenOf` resolve links through the
    `GuidIndex`, with a depth cap that also cuts off cycles in bad data.
  - A child whose parent doesn't currently exist behaves as a root, and is
    attached again the moment an entity with that GUID exists. So deleting a
    parent and undoing re-links its children with no cleanup code; the
    editor's orphan-unparenting and stale-link removal are gone.
- **`Transform` moved into the engine** (`scene/components.h`). It isn't
  physics-specific; `physics/components.h` includes it.
- **`ReparentCommand`** makes "Parent to selection" and "Unparent" in the
  Hierarchy panel undoable.
- **`CommitFieldEdit`**: turns any widget's in-place edit into an undoable
  command. Successive edits merge, and it breaks the merge chain on commit.
  - **Gizmo drags** are recorded as one undo step (start → end position)
    when the mouse is released.
  - **The body-list sliders** (position, radius, mass, static) go through it,
    and their Jolt syncing now happens in the hooks, so undo resyncs physics
    too.
  - `InspectEntity` uses it as well.
- **Test fix:** a test type added in step 3 (`cmd_test::Health`) shared its
  declared name with another test type. The registry was correctly rejecting
  it with an error log. It's renamed.

**Verified**: 6 new tests covering world transforms through a rotated
chain, cycle detection including a data cycle, a link surviving parent
destroy and recreate, `Parent` in both scene formats, reparent undo/redo
including delete-undo re-linking, and a gizmo-style drag becoming one undo
step. 112/112 tests pass on GCC 13, on Clang and under ASan/UBSan, and
119/119 with physics. The changed editor blocks (hooks, undo application,
gizmo drag, body list, Hierarchy panel, delete loop, hierarchy call sites)
were compiled together in a harness against the real headers, with no
warnings. The editor executable itself still needs a Windows build to
confirm.

**Step 5: Play-in-Editor** (`editor/src/core/play_session.h`).

- **Play** snapshots the edited level into memory (the binary scene format,
  via the new `SaveSceneToMemory`/`LoadSceneFromMemory`) and freezes the
  undo history.
- **Stop** throws the played world away and rebuilds the level from the
  snapshot. Everything that happened during play (physics, spawns,
  deletions, Inspector edits) is undone exactly, and the pre-play undo
  history still works.
  - This restores in place instead of running a separate play world as the
    spec describes. The result is the same for the user, and nothing that
    holds a `World&` has to switch worlds.
  - The hooks rebuild the Jolt bodies, and the selection carries over by
    GUID.
- **Pause, Resume and Step** (Step advances exactly one frame while paused).
- **`CommandStack::SetFrozen`**: during play, commands still apply but
  aren't recorded, and Undo/Redo do nothing.
- **Editor**:
  - The "Playing" checkbox is replaced by Play / Pause / Resume / Step / Stop
    buttons, with Alt+P to play or pause and Esc to stop, plus a status line
    saying play-time changes are discarded.
  - Save and Load Scene are disabled while playing.
  - **The editor now starts in edit mode**: nothing simulates until you press
    Play. Before, the physics ran from launch. Set
    `AETHER_EDITOR_AUTOPLAY=1` to start playing, for automated runs.
- Not included yet: `BeginPlay`/`EndPlay` events (they arrive with
  scripting in Phase 11) and the green PIE viewport border
  (`docs/design/EDITOR_UI.md` §6.2).

**Verified**: 3 new tests.

- A full play session (moving, spawning, deleting, plus an index gap left by
  earlier edits) is followed by Stop, and the level must save
  **byte-identical** to before Play. The test also checks that hooks fired
  and GUIDs and the hierarchy survived.
- History is frozen during play and works again after Stop.
- Pause and Step behave as described, and invalid transitions do nothing.
- 115/115 tests pass on GCC 13, on Clang and under ASan/UBSan, and 122/122
  with physics.
- The new editor blocks (session, stop-with-selection, shortcuts, toolbar,
  simulation gate, play/stop application) were compiled together in a
  harness with no warnings. The editor executable still needs a Windows
  build.

**Step 6: projects, and reflected arrays.**

- **Projects** (`engine/include/aether/project/project.h`, in the engine so
  the future standalone player can read them):
  - `ProjectSettings` is reflected and editable in the Inspector: name,
    engine version, startup scene, plugins, fixed timestep, gravity, and
    collision layers.
  - `CreateProject` makes the standard folder layout (`Content/`, `Config/`,
    `Saved/`, `Intermediate/`) with a `.gitignore` for the generated folders
    and writes `Name.aproject`. It validates the name and refuses a non-empty
    folder.
  - `LoadProject`/`SaveProject` use deterministic JSON stamped with the
    engine version (`aether/core/version.h`, now 0.7.0). Loading tolerates
    missing and unknown fields, warns about a project saved by a newer
    engine, and enforces "layer 0 is Default, at most 32 layers".
  - `EnsureProjectFolders` restores the ignored folders after a fresh clone.
- **Recent projects** (`editor/src/core/recent_projects.h`): most recent
  first, no duplicates, at most 10. It's stored per user (`%APPDATA%/Aether`
  or `~/.config/aether`), and can prune projects that no longer exist.
- **Reflected arrays**: `std::vector<T>` for any reflected `T` (structs and
  nested arrays included) is `TypeKind::Array`, with the declared name
  `Array<T>`.
  - JSON and binary archives handle arrays. The data decides the length, and
    each element loads tolerantly, with warnings naming the index
    (`Route.tags[1]`).
  - `Any` and field access work with arrays.
  - The Inspector shows arrays as a collapsible "N elements" list with a
    remove button per element and "+ Add".
- **Not in this step:** File > New / Open Project and the project browser
  window in the editor, which are Windows UI.

**Verified**: 9 new tests covering the array archives (nested, empty,
bad-element warnings), project create/load/save (layout, `.gitignore`,
byte-stable saves, bad names, non-empty folders, newer-engine and layer
warnings, non-project files), the recent-projects rules and persistence,
and a headless Inspector draw of `ProjectSettings`. 124/124 tests pass on
GCC 13, on Clang and under ASan/UBSan, and 131/131 with physics. Nothing in
the editor executable changed.

### Phase 8 (engine side done) — Asset System

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md), Phase 8.

**Step 1: asset IDs, `.ameta` files and the asset database**
(`engine/include/aether/assets/asset_database.h`).

- **`AssetGuid`**: a permanent version 4 UUID per asset, a separate type from
  `EntityGuid` so the two can't be mixed up. It's reflected and saved as its
  string form.
- **`.ameta` sidecars** (`Brick.png.ameta`, §A.2 format): GUID, importer,
  importer version, the source hash at the last import, importer-specific
  settings, and labels.
- **`AssetDatabase::Scan()`** walks `Content/`:
  - A source with no `.ameta` gets one, with a new GUID. Which files count
    as assets is decided by extension.
  - An `.ameta` whose source is gone is kept and marked missing, since the
    file may come back (a branch switch, say), and it keeps the same
    identity when it does.
  - A file copied together with its `.ameta` gives two assets the same GUID.
    The older `.ameta` keeps it and the newer one gets a fresh GUID, with a
    warning.
  - A source whose hash changed since its last import is marked
    `needs_import`, and `MarkImported` records the new hash.
  - An `.ameta` that can't be read is **left untouched** and its asset
    skipped, with a warning. Merge conflicts are the usual cause, and
    rewriting the file would lose the GUID.
  - Hidden folders such as `.git` are skipped, and paths are `/`-separated on
    every platform.
- **`Move()`** renames or moves an asset's source and `.ameta` together, so
  its GUID (and every reference to it) is unchanged. It refuses to overwrite
  an existing asset.
- File hashes are FNV-1a 64 for now (`fnv1a64:…`). xxHash is a planned
  speed upgrade, and the prefix keeps that change detectable.

**Verified**: 7 new tests. They cover:

- stable rescans with byte-identical `.ameta` files
- moves surviving a fresh database
- a missing source coming back with the same identity
- a duplicate GUID from copying a file with its `.ameta`
- change detection, with settings and labels preserved
- a corrupt `.ameta` being preserved

131/131 tests pass on GCC 13, on Clang and under ASan/UBSan, and 138/138
with physics.

**Step 2: asset references by ID, dependency tracking, safe delete.**

- **`AssetRef<T>`** (`assets/asset_ref.h`): a typed reference to an asset
  (`AssetRef<Model>`, `AssetRef<Texture>`, ...). It's reflected, and saved
  as the asset's GUID string (`""` for none), so it survives the asset being
  renamed or moved. The new `TypeInfo::asset_type` names the asset kind.
- **Dependencies**: `Scan` searches every scene and prefab, JSON or binary,
  for asset GUIDs (which is how `AssetRef` fields are saved).
  - `AssetRecord::dependencies` and `Referencers(guid)` answer "what uses
    this?".
  - Entity GUIDs share the format but aren't counted, because only known
    asset GUIDs are.
- **`Delete(guid, force)`** removes an asset's source and `.ameta`. It's
  refused while other assets use it, and the error names them.
- **`ModelRenderer.model`**: an `AssetRef<Model>` next to the existing
  `asset_path`, which is now shown read-only.
  - `ResolveModelAssets(world, db)` (`scene/model_assets.h`) links old
    path-only data to the GUID, and after a rename updates the path from the
    GUID, so moved models keep rendering. It won't accept a GUID that points
    at a non-model asset.
  - The editor's renderer still loads by path, which the resolver keeps
    current. Calling the resolver from the editor comes with its asset
    browser work (Windows).
- **Old scenes still load** through the new per-component
  `SetLegacyRawLoader`, since `ModelRenderer` changed size. It reads the old
  128-byte raw records, which the plain raw loader refuses because the sizes
  no longer match.
- **Inspector**: asset references are a dropdown of assets of that kind, from
  a list the editor supplies (`SetAssetListProvider`). A reference to an
  asset that's gone shows as "(missing …)".
- **Bug caught by AddressSanitizer and fixed before merging**: `Delete` (and
  `Move`) read their `guid` argument after erasing the record it pointed
  into (`db.Delete(db.FindByPath(p)->guid)`). They now copy it first.

**Verified**: 4 new tests covering the `AssetRef` JSON form (none, set,
bad), referencers from both scene formats (entity GUIDs excluded), rename
without touching scenes, refused and forced deletes, model resolution in
both directions, and the headless Inspector asset picker. The legacy
`ModelRenderer` scene test still passes. 135/135 tests pass on GCC 13, on
Clang and under ASan/UBSan, and 142/142 with physics.

**Step 3: importers, the derived data cache, and the texture importer.**

- **`IAssetImporter`** (`assets/importer.h`): an importer is a pure
  function of the source bytes and its settings, with a version number to
  bump whenever its output changes. `ImporterRegistry::WithBuiltins()`
  registers the built-in ones.
- **`ImportAsset`**:
  - Settings are the importer's defaults, with the `.ameta`'s `settings`
    merged over them.
  - It checks the cache for exactly that input before running the importer.
  - On success the `.ameta` records the imported source hash and importer
    version.
  - `ImportAll` handles everything marked `needs_import`, and reports what
    was imported, served from the cache, failed, or skipped (no importer for
    that type yet).
- **`DerivedDataCache`** (`assets/derived_data_cache.h`) stores entries under
  `Intermediate/DDC/`, with a 128-bit key from importer name, importer
  version, source hash, settings and platform.
  - Writes are atomic (a temp file, then rename).
  - Changing a setting and changing it back, switching branches, or a fresh
  clone therefore reuses earlier imports instead of redoing them.
- **`TextureImporter`**:
  - It decodes PNG/JPG/TGA/BMP to RGBA8.
  - It builds a full mip chain with a 2×2 box filter, averaging in linear
    light for sRGB textures (black and white averages to 188, not 128) and
    keeping alpha linear.
  - Settings: `srgb`, `generate_mips`, and `max_size` (halve until the image
    fits). A setting of the wrong type falls back to the default with a
    warning.
  - Output is the engine's `ATEX` format (`Encode/DecodeTextureData`, with
    size checks on load). It's uncompressed; BC7 needs an encoder, which
    comes later.
- **Tests import real files.** `AETHER_REPO_ASSETS_DIR` gives the tests the
  repo's `assets/` folder as an absolute path.

**Verified**: 5 new tests.

- A real 64×64 PNG yields a 7-level mip chain, and the 1×1 mip of the 2×2
  corners texture equals the linear-space average.
- Settings and `max_size` change the output as expected, bad settings give
  warnings, and a missing file or corrupt `ATEX` data is rejected.
- The cache is checked with an import-counting importer: a repeat import is
  a hit, a changed setting is a miss, changing it back is a hit again, and a
  different platform or an edited source is a miss.
- `ImportAll` over real textures plus a broken file and an unsupported type,
  then a cache-only pass after the stored hashes are wiped.
- 140/140 tests pass on GCC 13, on Clang and under ASan/UBSan, and 147/147
  with physics.

**Step 4: the glTF model importer, split into sub-assets.**

- **`ModelImporter`** (`assets/model_importer.h`) wraps the existing glTF
  loader. One `.gltf` becomes a model plus separately referenceable pieces:
  - `mesh:<i>`: a **Mesh** per glTF mesh, with vertices, indices,
    skinning data and per-primitive bounds. It's saved in the compact
    binary `AMSH` format (`Encode/DecodeMeshData`, size-checked on load).
  - `material:<i>`: a **Material** per material (metallic-roughness), saved
    through reflection. Texture URIs are rewritten as content-relative asset
    paths (`../textures/a.png` becomes `textures/a.png`), ready for
    `FindByPath`.
  - `animation:<i>`: an **Animation** per clip, saved through reflection.
  - The model's own data is a manifest: the node tree, skins, and the keys
    of its sub-assets.
  - Settings: `import_materials` and `import_animations`.
- **Sub-assets in the database**:
  - Each one gets its own GUID, kept in the source's `.ameta` under
    `sub_assets`, so the GUID is stable across reimports and sessions.
    `AssetRef<MeshAsset>`, `AssetRef<MaterialAsset>` and
    `AssetRef<AnimationAsset>` can point at them.
  - Their records have paths such as `models/hero.gltf#mesh:0`, a `parent`,
    and a `sub_key`.
  - `SetSubAssets` keeps the GUIDs of existing keys, adds new keys, and
    removes keys the importer no longer produces (for example, after
    turning materials off).
  - Moving a model moves its sub-assets' paths with it. Deleting a model is
    refused while anything uses the model **or any of its pieces**.
    Sub-assets can't be moved or deleted on their own.
  - Copying a model together with its `.ameta` gives the copy new GUIDs for
    the model and every sub-asset.
- **`ImportAsset`**:
  - Cache entries now hold the main data plus every sub-asset (`AIMP`
    container). A cached entry in the older layout is simply re-imported.
  - Called with a sub-asset's GUID, it imports the source (normally a cache
    hit) and returns just that piece.

**Verified**: 5 new tests, using the repo's glTF files.

- `AMSH` round-trips, and truncated data, trailing bytes and oversized
  counts are rejected.
- The textured cube has 24 vertices, 36 indices, symmetric bounds, and its
  texture path rewritten.
- Animation keys have the right width, skinned vertices carry joints, the
  node tree is kept, settings drop the optional pieces, and a missing file
  fails.
- Sub-asset GUIDs are the same in a fresh database and after a cached
  reimport, and a changed setting removes only the material.
- Move and delete work with sub-assets, including a scene that references
  a mesh blocking deletion, and copying with the `.ameta` gives fresh GUIDs.
- UBSan caught a zero-length `memcpy` from an empty vector's null
  pointer, which was fixed before merging.
- 145/145 tests pass on GCC 13, on Clang and under ASan/UBSan, and 152/152
  with physics.

**Step 5: file watching, debouncing and hot reload.**

- **`FileWatcher`** (`assets/file_watcher.h`) reports files added, modified
  or removed under `Content/`.
  - A change is reported once it has been quiet for 200 ms, so a burst of
    saves is one change.
  - Editors that save by writing a temp file and renaming it over the
    original produce a single Modified for the original. The temp file's add
    and remove cancel out.
  - It polls (modification time and size), which works on every platform.
    Native notifications (`ReadDirectoryChangesW`, `inotify`) can replace
    the folder walk later without changing the interface.
  - Time is passed in, so tests control it exactly.
- **`AssetHandle<T>` / `AssetStore<T>`** (`assets/asset_handle.h`):
  - Handles share one slot per asset. A reload replaces the data behind
    every handle and bumps a generation number.
  - Code that built something from the data, such as a GPU texture,
    compares generations and rebuilds when they differ.
- **`HotReloader`** (`assets/hot_reload.h`): call `Update(now)` once per
  frame. It rescans and reimports what changed, and returns what happened:
  - **Reimported**: the source was edited, it came back after being deleted,
    or its import settings were edited by hand in the `.ameta`.
  - **Added**: a new file, imported straight away.
  - **Failed**: the import failed; keep using the old data. It's reported
    once and retried only when the file or its settings change, not on
    every later change.
  - **Moved**: renamed or moved outside the editor together with its
    `.ameta`. Same GUID, so nothing that uses it breaks.
  - **Missing**: its source was deleted.
  - Each change lists the assets that use it (including through its
    sub-assets), for rebuilding things like a material after its texture
    changes.
  - Importing rewrites the `.ameta`. The reloader tells the watcher about
    its own writes, so they don't come back as changes and cause endless
    reimports.
  - Imports run inside `Update` for now. They move to worker jobs when the
    engine has a job system.
  - Hooking this into the editor's renderer (textures, then meshes, then
    shaders) needs the Windows build, like the other editor-side work.

**Verified**: 3 new tests.

- **Watcher**: debounce timing, the temp-file-and-rename save, removals,
  hidden folders, and a file created and deleted before settling.
- **Handles**: see reloads and keep their data after the store drops the
  asset.
- **End to end, on a real texture**:
  - An edit is reimported and the handle's generation goes up, with the
    scene that uses it listed.
  - The reloader's own writes cause no further changes.
  - A settings edit reimports without mips; rewriting the same settings
    does nothing.
  - A new file is added and a broken file fails.
  - A move outside the editor keeps the GUID.
  - A delete is reported missing, and the file coming back is reimported.
  - Fixing the broken file imports it.
- **Bug caught by the end-to-end test and fixed before merging**: a file
  that failed to import was retried and reported again on every unrelated
  change.
- 148/148 tests pass on GCC 13, on Clang and under ASan/UBSan, and 155/155
  with physics.

**Step 6: the Content Browser's engine side** (`assets/content_browser.h`).
The ImGui panel draws from this. The panel itself is editor work pending the
Windows build.

- **Listing**: `ContentFolders` lists every folder, including empty ones.
  - `ListContent` shows folders first, then assets, sorted by name ignoring
    case.
  - Search matches part of a name, ignoring case, and looks through
    subfolders.
  - Results can be filtered by type, and a model's meshes, materials and
    animations can be listed with it.
  - `CreateContentFolder` makes a new folder.
- **Names**: `IsValidAssetName` accepts only names that work on every
  platform. It refuses Windows-forbidden characters, `#` (used for
  sub-asset paths), hidden names, a trailing dot or space, and device names
  such as `CON`.
- **Rename and move with reference safety**: `MoveContent` and
  `RenameContent` move a file or a whole folder, including its `.ameta`
  files and helper files such as a glTF's `.bin`.
  - GUID references (scenes, prefabs, `AssetRef`) need no change.
  - glTF files refer to their buffers and textures by **relative path**,
    which a move would break, so those paths are rewritten. This covers a
    moved model's own references and other models' references to a moved
    texture.
  - Rename keeps the extension and refuses to change it, since that would
    change how the file is imported.
  - Moves are refused into the folder itself, onto an existing file, for a
    sub-asset or an `.ameta`, and outside `Content/`.
- **glTF dependencies**: a model now depends on the texture assets it names,
  so deleting a texture a model uses is refused, and hot reload lists the
  model among the texture's dependents.
- **Reference Viewer**: `CollectReferences` walks what an asset uses, or what
  uses it (including through its sub-assets), depth-first. A shared asset or
  a cycle is shown once and marked, not followed again.
- **Thumbnails**: `AssetThumbnail` works for textures and materials.
  - A texture's thumbnail is made from the smallest mip that's still large
    enough, then box-filtered to fit.
  - A material's thumbnail is its base colour texture, or a colour swatch,
    tinted by the base colour in linear light.
  - Models and meshes need the renderer, so they show a type icon instead.

**Verified**: 5 new tests.

- **Listing**: folders (empty ones included, hidden ones skipped), search,
  the type filter, and sub-assets.
- **Names**: invalid names are refused, and URI encoding round-trips.
- **Moves**:
  - Moving a texture rewrote the 3 models that use it, and the material then
    imports with the new path.
  - A model moved on its own still loads its 24-vertex mesh.
  - A folder move carries its `.bin`, and references from outside into the
    folder are updated.
  - Renames, including a folder rename, keep GUIDs.
  - Seven kinds of invalid move are refused.
- **Reference Viewer**: both directions, with a scene cycle.
- **Thumbnails**: scaling, a solid-red texture, a material, and no thumbnail
  for models and meshes.
- **Test bugs fixed**: searching "red" also matched "texturED", and one test
  looked up sub-assets before importing anything.
- 153/153 tests pass on GCC 13, on Clang and under ASan/UBSan, and 160/160
  with physics.

### Phase 9 (engine side done) — Prefabs and scheduling

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md), Phase 9.

**Step 1: prefab assets and resolving instances** (`scene/prefab.h`).

- **Prefab data** (`.aprefab`, JSON): a root entity and its descendants.
  - Each entity has a local ID that's unique within the prefab, its
    parent's local ID, and its reflected components in the same JSON form
    as JSON scenes.
  - `MakePrefab` captures an entity and everything under it.
  - Loading checks the structure: one root, unique non-zero IDs, and
    parents listed before their children.
- **Instances**: the root entity of an instance has a `PrefabInstance`
  component. It holds the prefab reference, the property overrides, and
  the prefab entities this instance removed.
  - Every entity created from the prefab gets a `PrefabLink` (its instance
    and local ID).
  - `InstantiatePrefab` creates an instance, optionally at a given
    Transform.
- **Overrides** name an entity, a component, a field path (`gold`,
  `items[1]`, `position[1]`, or `""` for the whole component) and a JSON
  value.
  - Whole-component overrides are applied first, so a field override on the
    same component wins.
  - An override whose entity, component or field no longer exists is
    **kept and reported as orphaned**, not silently deleted.
- **`ResolvePrefabInstance`** (§9.2): starts from the prefab's data, takes
  away the removed entities, applies the overrides, then creates, updates or
  destroys entities to match.
  - Existing entities are **reused** (matched by their `PrefabLink`), so
    references and selections stay valid.
  - The root keeps its own Transform and parent: that's where the instance
    is placed.
  - Children added to an instance by hand have no link, so they're left
    alone.
  - `ResolveAllPrefabInstances` re-resolves every instance after a scene
    loads or a prefab changes, and reports instances whose prefab isn't
    available.
- **Scene loading registers the engine's scene components first.**
  Components get their IDs on first use, so a scene loaded before any
  prefab code had run would have dropped `PrefabInstance` data as unknown.
  The same applied to `Parent`.
- Nesting and variants come in step 3, and recording overrides from editor
  edits (with bold fields, Apply and Revert) in step 2.

**Verified**: 5 new tests.

- **Capture and files**: capturing a 4-entity chest, a file round trip, and
  four kinds of malformed prefab refused.
- **Instantiating**: the hierarchy is built, world positions follow the
  placed root, and each instance gets separate entities.
- **One override among 100 instances**: when the prefab changes the same
  field, 99 instances follow and the overridden one keeps its value, on the
  same entity. Removing the override brings it back in line.
- **Override paths**: vector and array elements, whole-component
  precedence, and 4 kinds of orphan, all kept on the instance.
- **Removed and added children**: they survive binary and JSON scene
  save/load plus prefab edits (a new child, a changed field, a deleted
  entity, a dropped component), and an unavailable prefab is reported.
- 158/158 tests pass on GCC 13, on Clang and under ASan/UBSan, and 165/165
  with physics.

**Step 2: recording overrides, marking them, Apply and Revert** (§9.3).

- **`RecordPrefabOverrides`** compares an instance entity's component with
  the prefab and stores the differences as per-field overrides: `gold`,
  `position[1]`, or a whole list that changed length.
  - Editing a field back to the prefab's value removes its override.
  - Moving an instance's root is placing it, not overriding it.
  - Orphaned overrides are kept.
- **`SetFieldCommand` records overrides** whenever the editor supplies
  `CommandContext::find_prefab`. It records on Do, Undo and Redo, so undoing
  an edit also undoes its override.
- **`RevertPrefabOverrideCommand`** (Revert to Prefab, for one field or all
  of an entity's overrides) removes the overrides and resolves the
  instance. Undo puts both the values and the overrides back.
- **`ApplyOverridesToPrefab`** writes selected overrides into the prefab and
  removes them from the instance. Other instances that override the same
  field keep their own values. Orphans can't be applied and stay.
- **`IsFieldOverridden`** and **`RevertOverrides`** support the UI.
  Overrides are matched by path, so `position` covers `position[1]`.
- **Inspector**: overridden fields get an accent bar and a highlighted
  label (`InspectOptions::is_overridden`), plus a right-click
  **Revert to Prefab**. An instance's entity shows its override count and a
  **Revert All** button.
  - Apply to Prefab needs the prefab asset saved, so its button comes with
    the editor's asset work on Windows.
- `CommandContext` gained a constructor, so existing `{world, guids,
  &hooks}` initialisations don't trip missing-initialiser warnings.

**Verified**: 4 new tests.

- **Recording**: a field, reverting it by hand, a vector element, a
  resized array, root placement, a non-instance entity, and a surviving
  orphan.
- **Apply and Revert across three instances**: the others follow the new
  default and the overriding one keeps its value. Path matching is
  checked.
- **Editor commands**: an edit records an override, and undo, redo and
  editing back all behave. Revert and Revert All work and undo, and
  nothing is recorded without a prefab lookup.
- **Headless Inspector**: draws a marked instance without recording
  anything, and only top-level fields are asked about.
- 162/162 tests pass on GCC 13, on Clang and under ASan/UBSan, and 169/169
  with physics.

**Step 3: nested prefabs, variants and cycle detection.**

- **Nested prefabs**: a prefab entity can be an instance of another prefab.
  It carries that prefab's source, its overrides and its removed entities.
  - The nesting entity's own components (typically its Transform) apply on
    top of the nested root's.
  - `MakePrefab` turns an instance it captures into a nested entry, instead
    of copying its entities.
- **Variants**: a prefab can be a variant of another (`base`). It changes
  the base's entities with its own overrides and removals, and can add
  entities.
- **`FlattenPrefab`** resolves nesting and variants into one plain entity
  list, which is what instances are resolved from.
  - Precedence runs as in the spec: nested defaults, then the nesting
    prefab's overrides, then variant overrides, then instance overrides.
  - Entities inside nested prefabs get **stable local IDs** derived from
    their path (`NestedLocalId`), so instance overrides and entity reuse
    work for them unchanged.
  - Refused with the prefab chain named: **cycles** (a prefab containing
    itself, or a variant of itself, directly or not), missing prefabs,
    nesting more than 32 deep, clashing IDs, and a variant entity whose
    parent was removed.
  - `ResolveAllPrefabInstances` flattens each prefab once per call. An edit
    to the innermost prefab reaches every instance and reuses the same
    entities.
- **Apply to a variant** turns overrides of the base's entities into
  variant overrides, leaving the base untouched. Applying overrides of
  entities inside nested prefabs is not done yet: they stay on the
  instance.
- The `.aprefab` format adds `"prefab": {source, overrides, removed}` on
  nested entities and a top-level `"base"` for variants.

**Verified**: 3 new tests.

- **Three levels deep**: an override at each level wins over the one
  below. They peel off one by one, an edit to the innermost prefab
  propagates, and a nesting entity can place the nested root.
- **Variants**: a variant of a variant with removal, base changes flowing
  through, Apply into the variant, and an error for a removed parent.
- **Errors**: a two-prefab nesting cycle, a direct and a three-step
  variant cycle, and a missing prefab (also through `ResolveAll`).
- **Files and capture**: capturing a scene with an instance, JSON round
  trips of nested entries and variants, and malformed files refused.
- 165/165 tests pass on GCC 13, on Clang and under ASan/UBSan, and 172/172
  with physics.

**Step 4: propagation, broken-instance warnings, and migrating overrides.**

- **`PrefabLibrary`** (`scene/prefab_library.h`) loads prefab assets from
  the asset database on first use and caches their flattened form.
  - `Save` writes a prefab and drops the cached flat data of every prefab
    built on it (nesting it or a variant of it, at any depth).
  - `Reload` does the same after the file changed on disk.
  - `Dependents` works that set out from the prefab data itself.
- **`PropagatePrefabChange`** re-resolves only the instances of the changed
  prefab and of prefabs built on it. Entities are reused, so selection and
  references survive. It counts instances whose overrides were orphaned
  and instances that failed (a cycle introduced by the edit, say).
- **`CheckScenesUsingPrefab`** gives the §9.4 warnings. It lists every saved
  scene that uses the prefab (directly or through prefabs built on it),
  loads each into a scratch world, and reports the overrides the change
  broke. Scene files aren't modified.
- **Hot reload** reports a new **Changed** event for assets without an
  importer (prefabs, scenes, scripts) when their content really changes.
  The editor then calls `PrefabLibrary::Reload` and propagates.
  - Assets without an importer are never marked imported, so change
    detection compares their content hash, not the `needs_import` flag.
- **Overrides migrate with their component** (§9.5).
  - Overrides now record the component's schema version.
  - When a C++ type is migrated (for example a field renamed from `gold` to
    `coins`), resolving runs the type's migration hook on the overridden
    field alone. This happens twice: once with a marker value to find the
    field's new path, and once with the real value to get its new form. The
    migrated override is saved back on the instance.
  - Prefab data is compared and edited in the current schema version when
    recording overrides and applying them.
  - New `reflect::MigrateJson` brings saved JSON up to date without loading
    it.

**Verified**: 4 new tests.

- **Propagation across a nested prefab and a variant**: 3 instances follow
  and an unrelated prefab's instance is untouched. The overriding
  instance keeps its value on the same entity, and the file is saved.
  A breaking change orphans the override (it's kept), and a cycle
  introduced by an edit fails all 3 instances, naming the cycle.
- **Scene reports**: the scene using a variant is listed, a breaking change
  reports its override, and an unrelated binary scene isn't listed.
- **Hot reload**: an external edit to a `.aprefab` arrives as one Changed
  event and reaches the instance.
- **Migration**: a v1 `gold` override becomes a v2 `coins` override in
  place, resolves against old and re-saved prefab data alike, and is
  recorded in the new version. It also covers whole-component and
  unversioned overrides, and Apply into an old prefab.
- **Bug caught before merging**: the first version of the Changed event
  fired for every importer-less asset on any file change, because such
  assets always count as needing import.
- 169/169 tests pass on GCC 13, on Clang and under ASan/UBSan, and 176/176
  with physics.

**Step 5: gameplay framework components and lifecycle events.**

- **`scene/gameplay.h`**:
  - **`Camera`**: perspective or orthographic, FOV, orthographic height,
    near and far planes, and priority.
    - `CameraProjection` works for both modes (the new `OrthographicRH` is
      the counterpart of `PerspectiveRH`, with Vulkan's 0–1 depth).
    - `CameraView` inverts the camera's world transform, through its
      parents.
    - `FindActiveCamera` picks the highest-priority camera on an active
      entity.
    - Degenerate settings are clamped rather than producing NaNs.
  - **`Tags`**: named labels, with `HasTag`, `AddTag`, `RemoveTag` and
    `FindEntitiesWithTag`.
  - **`Layer`**: an index into the project's 32 named layers, with
    `LayerMask`, `FindLayer`, `MakeLayerMask` (unknown names are reported)
    and `IsInLayerMask`.
  - **`Active`**: switches an entity and everything under it on or off
    (`IsActiveInHierarchy`).
  - All four are reflected (so they appear in the Inspector) and are
    registered before scenes load.
- **`Lifecycle`** (`scene/lifecycle.h`) runs `OnCreate`, `OnEnable`,
  `OnStart`, `OnUpdate`, `OnFixedUpdate`, `OnLateUpdate`, `OnDisable` and
  `OnDestroy` for the components they're registered on. This is what
  scripts and Blueprints will attach to.
  - Ordering: creating, enabling and starting go parents first. Disabling
    and destroying go children first.
  - Deactivating an entity disables its subtree and stops its updates.
    Reactivating it enables them again.
  - `Destroy` removes a whole subtree.
  - Anything that happens during a callback takes effect when the current
    pass ends, so callbacks never see half-destroyed state. That covers a
    spawn, a `Destroy`, a `SetActive`, and components being added or
    removed. An entity queued for destruction still gets that pass's
    update.
  - Removing a component ends just its registration, and adding one during
    play starts it.

**Verified**: 5 new tests.

- **Camera maths**: near and far planes map to depth 0 and 1, the
  frustum's top edge maps to +1, orthographic corners and depth are
  checked, and view × world is the identity through a rotated parent.
- **Active-camera choice**: priority, inactive entities and inactive
  parents, and clamping of degenerate settings.
- **Tags and layers**: exact name matching, masks, out-of-range layers,
  and a scene save/load round trip of all four components.
- **Lifecycle**: the full event order on a three-level hierarchy through
  play, deactivate, reactivate, destroy and end of play. Spawns, destroys
  and deactivation made during a callback are safe, and the
  per-component registrations are covered.
- 174/174 tests pass on GCC 13, on Clang and under ASan/UBSan, and 181/181
  with physics.

**Step 6: the system scheduler and the fixed-timestep frame**
(`scene/scheduler.h`, §9.6).

- **`SystemDesc`**: a name, a phase (PreUpdate, FixedUpdate, Update,
  LateUpdate or PreRender), the components it reads and writes, `after`
  constraints, and whether it's `main_thread_only`.
- **`SystemScheduler`** builds one dependency graph per phase.
  - A system runs after the ones it lists in `after`, and after earlier
    systems it conflicts with: two systems that touch the same component,
    at least one writing it.
  - The order behind "earlier" comes from the `after` lists first, then
    registration order. So only contradictory `after` lists can make a
    cycle.
  - It reports an `after` that names an unknown system, one in another
    phase, and cycles (naming the systems in the cycle).
- **Running a phase**: the graph is run as levels of mutually independent
  systems.
  - With a `JobSystem`, each level runs in parallel on its workers.
    `main_thread_only` systems run on the calling thread at the same time.
  - Without one, a phase runs in its sequential order.
  - Levels are a conservative way to schedule the graph. Starting each
    system the moment its own predecessors finish can come later without
    changing the interface.
- **`FixedTimestep`**: an accumulator that runs 0–5 fixed steps a frame. If
  more are owed (a hitch), the excess is dropped, which avoids the "spiral
  of death". `Alpha()` is the blend factor for rendering.
- **`FrameLoop::Tick`** runs PreUpdate, FixedUpdate as many times as due,
  Update, LateUpdate, then PreRender with `alpha` set.
- **Render interpolation**: before each fixed step, `PreviousTransform`
  records each entity's Transform (`RecordPreviousTransformsSystem`).
  `InterpolateTransform` blends position and rotation (shortest-path nlerp)
  by `alpha`, so movement is drawn smoothly between fixed steps.

**Verified**: 5 new tests.

- **Ordering**: levels and order from access and `after`, readers sharing a
  level, and `after` overriding registration order. Four kinds of bad
  graph are rejected.
- **Parallel runs**: each system registers its access while it runs, and
  no conflicting pair was ever seen running at once. Independent systems
  did overlap, the main-thread-only system stayed on the main thread, and
  a run without a job system matches the sequential order. Stable across
  repeated runs.
- **Fixed timestep**: accumulation, the 5-step cap with drop, and 144 Hz
  frames averaging out to 60 fixed steps a second.
- **Frame loop**: phase order and fixed-step counts, a mover drawn at
  `alpha` 0.5 between steps, and shortest-path rotation blending.
- 179/179 tests pass on GCC 13, on Clang and under ASan/UBSan, and 186/186
  with physics.

With this, Phase 9 is done on the engine side. Its editor parts (prefab
edit mode, the Apply button, the warnings in the Messages panel) wait for
the Windows build along with the Phase 7 and 8 editor work.

### Phase 10 (in progress) — Input

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md), Phase 10.

**Step 1: action mapping** (`input/keys.h`, `input/actions.h`).

- **`Key`** covers keyboard, mouse buttons, mouse movement and wheel,
  gamepad buttons, sticks and triggers. Keys are saved by name (`"W"`,
  `"GamepadA"`).
- **`InputState`** is the raw device layer. The platform feeds it, and so
  can tests with synthetic events. Mouse movement accumulates over a frame,
  and `EndFrame` resets it.
- **Actions and mapping contexts** (Unreal Enhanced Input style):
  - An `InputAction` has a name and a value type (Bool, Axis1D/2D/3D).
  - An `InputMappingContext` binds keys to actions.
  - **Modifiers**: dead zone, negate, swizzle, scale. WASD becomes one 2D
    Move axis this way.
  - **Triggers**: Down, Pressed, Released, Hold (once or repeating), Tap,
    DoubleTap, and Chord (for example, W sprints only while Shift is
    held).
- **Context stack with priorities**: a vehicle context's Space can mean
  Brake, hiding the on-foot Jump, and a UI context can block gameplay
  entirely. A key lost to a higher context resets quietly, so closing a
  menu doesn't fire spurious Released or Completed events.
- **Reading actions**: `GetBool` / `GetAxis1D` / `GetAxis2D` / `GetAxis3D`,
  and events (Started, Ongoing, Triggered, Completed, Canceled) through
  `Subscribe`. The event names match what the Blueprint nodes will expose.
- All input types are reflected, so mapping contexts save as readable JSON.
- Still to come: input assets, rebinding (step 2), the Win32 and XInput
  backends (step 3), and the editor (step 4).

**Verified**: 6 new tests, all on synthetic device events (the phase's
"done when").

- **Device state**: key names and resetting deltas.
- **Modifiers**: WASD to 2D, keys cancelling out, a stick through a dead
  zone and scale, and swizzle orders.
- **Triggers**, over 0.1 s frames:
  - Pressed and Released.
  - Hold firing on the 6th frame and completing while still held.
    Released early, it's canceled. A repeating hold fires every frame.
  - Tap, fast and too slow; DoubleTap, fast and too slow; Chord.
- **Context stack**: consuming, non-consuming, UI blocking with no
  spurious events afterwards, mouse movement as an axis, and
  subscriptions.
- **JSON**: a mapping context round-trips byte for byte.
- 185/185 tests pass on GCC 13, on Clang and under ASan/UBSan, and 192/192
  with physics.

**Step 2: input assets, rebinding and key capture** (`input/bindings.h`).

- **Input assets**: `.aaction` (an action) and `.amapping` (a mapping
  context) are reflected JSON files in `Content/`.
  - The asset database recognises both.
  - `InputAssetLibrary` loads every one, reports unreadable files, registers
    the actions, and activates contexts by name.
  - A context saved without a name takes its file's name.
- **Rebinding per player**: a `KeyOverride` is "the Nth binding of this
  action in this context now uses this key", and `Key::None` unbinds it.
  - `UserBindings` holds a player's rebinds.
  - `ApplyUserBindings` gives a rebound copy of a context. The project's
    defaults are never changed, and a rebind whose binding no longer exists
    is reported, not applied.
  - Rebinds are saved in `Saved/Config/Input.json` (`UserBindingsPath`),
    separate from the project's defaults, so the defaults can change
    without losing the player's choices.
  - `FindConflicts` lists the other bindings already using a key, for "C is
    already bound to Crouch" warnings.
- **"Press a key…" capture** (`KeyCapture`):
  - It ignores the button that was held when capture began (the click on
    the button itself) until it's released.
  - Mouse movement never counts. Sticks and triggers count once pushed past
    half-way.
  - Escape cancels, or can be allowed as a binding.

**Verified**: 3 new tests.

- **Assets**: saved and loaded through the database with extensions
  recognised, a broken file reported, an unnamed context named after its
  file, and a wrong-kind file refused. An activated asset context drives
  an action.
- **Rebinding**: slots, conflicts, a replaced, unbound, stale or
  other-context rebind, the old key no longer working in play, and saving
  and reloading with keys stored by name. Reset one and reset all, with no
  file meaning no rebinds.
- **Key capture**: the held click, mouse movement, a half-pressed trigger,
  Escape both ways, and re-pressing after release.
- 188/188 tests pass on GCC 13, on Clang and under ASan/UBSan, and 195/195
  with physics.

Steps 3 (Win32 and XInput backends) and 4 (the input editor) need the
Windows build.

### Phase 11 (done on the portable side) — Scripting (Luau)

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md), Phase 11.

**Step 1: the Luau VM** (`scripting/`, the `Aether::Scripting` library).

- **Luau** (MIT) is fetched at configure time (tag `0.740`), behind the new
  `AETHER_BUILD_SCRIPTING` option, which is on by default.
  - Only its VM and compiler are built.
  - Its code isn't held to the engine's warning flags.
  - Luau's headers stay private to the scripting library.
- **`LuauHost`** compiles and runs chunks and files, and calls global
  functions with values (nil, bool, number, string).
  - It reads and sets globals, and caches compiled bytecode by source
    hash. Failed compiles aren't cached.
  - Errors come back as `name:line: message`, and the VM stays usable
    after one.
  - `print` goes to a handler, which by default is the engine log.
- **Sandbox (§11.4)**: no `io`, `os` reduced to `os.clock`, no `loadstring`,
  and no `debug` unless allowed (editor builds, for the debugger).
- **Limits**:
  - An instruction budget per run or call, through Luau's interrupt hook.
    An endless loop fails with a clear error instead of hanging.
  - A memory cap, through the VM's allocator. After any failed call the
    garbage is collected, so a script that hits the cap doesn't leave the
    VM full.

**Verified**: 3 new tests.

- **Running and calling**: chunks and calls with typed results, globals
  across runs, the bytecode cache, and print formatting.
- **Errors**: syntax and runtime errors with line numbers, `error()`,
  recovery afterwards, and running files.
- **Sandbox and limits**: every removed library, the allowed ones, debug
  in editor mode, the instruction budget (reset per run), and the memory
  cap with recovery.
- **Bug caught before merging**: after hitting the memory cap, the next run
  failed too, because the failed script's garbage was still counted.
- 191/191 tests pass on GCC 13, on Clang and under ASan/UBSan, and 198/198
  with physics.

**Step 2: reflection-driven bindings** (`LuauHost::BindWorld`, §11.1).

- Scripts get a **`world`** (`Spawn`, `Destroy`, `Find(guid)`,
  `EntitiesWith(name)`) and **entities** with `:Get`, `:Add`, `:Has`,
  `:Remove`, `:IsValid` and `:Guid`.
- **Components** expose their reflected **fields** for reading and
  writing, and their reflected **functions** as methods
  (`health:Heal(25)`, through `FunctionInfo::Invoke`).
  - Fields are looked up in a precomputed name map per type.
  - There are fast paths for bool, every integer size (whole numbers only,
    no negatives for unsigned), floats, strings, fixed-size strings
    (truncated to fit), enums (by name) and `Vec3`.
  - `Vec3` is Luau's native vector, so
    `vector.create(1, 2, 3) * 2 + v` works.
  - Structs and arrays go through the reflection JSON form as tables.
  - A wrong type is a script error naming the field ("Health.max expects a
    number, got string"), and nothing is written.
- **Handles, never pointers**: entities and components carry the entity
  and a binding generation, and are re-checked on every use. Each of these
  is a clear script error rather than a crash or a read of freed memory:
  - a destroyed entity, even one whose slot was reused
  - a removed component
  - a world that was re-bound or unbound
- Entities also cross between C++ and scripts as `EntityRef` values, in
  both directions.
- Unreflected components can be tested for and removed by their
  registered name, but not read.

**Verified**: 3 new tests.

- **Fields**: every kind of field read and written, vector maths,
  truncation, and five kinds of refused write.
- **Methods, entities and the world**: methods with argument checking, and
  spawning, adding, finding, listing and removing. Entities are passed
  both ways, and unreflected components are handled as described.
- **Handle safety**: a removed and re-added component, an entity destroyed
  and its slot reused, re-binding, unbinding, and a no-world entity
  arriving as nil.
- ASan's leak checker confirms that script errors raised from C++ binding
  code unwind cleanly (Luau uses C++ exceptions here).
- 194/194 tests pass on GCC 13, on Clang and under ASan/UBSan, and 201/201
  with physics.

**Step 3: script components, the script lifecycle, and exposed variables**
(§11.2).

- **`ScriptComponent`** (`scene/script_component.h`, in the engine so scenes
  load it without the scripting library) references a `.luau` script asset
  and stores the entity's overridden variables as JSON values.
- **`ScriptSystem`** (scripting library) plugs into the Phase 9 `Lifecycle`.
  - A script is a module that returns a table, its "class". Each entity
    gets its own instance, with `self.entity` set and its overrides applied.
  - The class's `OnCreate`, `OnEnable`, `OnStart`, `OnUpdate(dt)`,
    `OnFixedUpdate(dt)`, `OnLateUpdate(dt)`, `OnDisable` and `OnDestroy`
    are called when defined.
  - `CallMethod` and `GetField` reach an entity's instance from C++, for
    events later.
  - A runtime error in one script is recorded with its location, and every
    other script keeps running.
  - An override the script doesn't have, or of the wrong type, is reported
    and the default is used. So is a missing script.
- **Exposed variables**: the class's top-level number, bool, string and
  vector fields.
  - `--@range a b` and `--@tooltip text` comments above a field add
    metadata.
  - Fields starting with `_` stay private, and tables aren't exposed.
  - `DescribeSource` lists them, along with the callbacks the class
    defines.
- **Inspector** (`editor/src/ui/script_inspector.h`, portable, tested
  headless):
  - A slider (clamped to the range), checkbox, text field or X/Y/Z row per
    variable, showing the default until overridden.
  - Overrides are marked like prefab overrides and have a right-click
    **Reset to Default**. Typing the default back removes the override.
  - A script with an error shows the error instead.

**Verified**: 4 new tests.

- **Describing a class**: variables, metadata, private fields and tables,
  callbacks, and a script that returns something other than a table or has
  a syntax error.
- **Lifecycle on two entities with different overrides**: event order,
  each entity moved by its own speed, independent per-instance state, a
  method call, and destroy mid-play plus end of play releasing instances.
- **Errors**: bad overrides and a missing script reported, and a runtime
  error in one script not stopping another. The component survives a scene
  round trip.
- **Headless Inspector**: drawing records nothing, typing makes an override
  and typing the default back removes it, a wrongly typed stored value
  falls back to the default, and errors are displayed.
- **Found while testing**: the variable text field appended to its text on
  re-activation instead of replacing it. It now selects all when
  activated.
- 198/198 tests pass on GCC 13, on Clang and under ASan/UBSan, and 205/205
  with physics.

**Step 4: events, timers and input for scripts** (`scripting/src/script_api.cpp`).

- **Events**:
  - `Event.new()` gives an event, `event:Connect(fn)` a connection (with
    `:Disconnect()` and `:IsConnected()`), and `event:Fire(...)` calls
    every handler.
  - A failing handler is reported and the rest still run.
  - A handler can disconnect itself, or others, while the event is firing.
- **Timers**:
  - `Timer.After(s, fn)` runs once and `Timer.Every(s, fn)` repeats. Both
    have `:Cancel()`.
  - They run in game time through `ScriptSystem::Tick(dt)`.
  - A long frame lets a repeating timer catch up (up to 100 runs a frame),
    so `Every(1)` counts real seconds.
- **Input** (bound with `BindInput`):
  - `Input.IsTriggered` and `Input.GetAxis1D` / `GetAxis2D` / `GetAxis3D`
    read actions, returning vectors for axes.
  - `Input.OnStarted` / `OnTriggered` / `OnCompleted` / `OnCanceled(action)`
    are events fired with the action's value.
  - Without a bound input system, everything reads as released.
- **Ownership (§11.2)**: connections and timers made while an entity's
  callbacks run belong to that entity's script. They end when it's
  destroyed, and ones made at top level stay.
- **Entity events**: `SendEvent(entity, "OnHit", args)` calls a method on
  the entity's script if it defines one. Physics collisions and triggers
  will use this in Phase 13, along with raycasts and other physics
  queries.

**Verified**: 4 new tests.

- **Events**: connecting, firing with arguments, disconnecting and
  checking, a failing handler, a self-disconnecting handler, and argument
  checks.
- **Timers**: once, repeating with catch-up, a failing timer, cancelling,
  and an invalid interval.
- **Ownership**: an entity's connection and timer end when it's destroyed
  while another entity's continue. Entity events are covered.
- **Input**: events and axis polling from scripts, the unbound case, and
  unbinding.
- 202/202 tests pass on GCC 13, on Clang and under ASan/UBSan, and 209/209
  with physics.

**Step 5: script hot reload and the error overlay** (§11.3).

- **`ScriptSystem::Reload(script)`** recompiles a script while the game
  runs.
  - **If it fails**, running instances keep the old code. The error is
    kept, split into chunk, line and message (`ScriptError`), for the
    overlay until a later reload succeeds.
  - **If it succeeds**, every live instance keeps its own fields (its
    state) but switches to the new class, and `OnReload` is called if the
    class defines it. New methods apply straight away, and so do new
    defaults for variables the entity doesn't override. Its own overrides
    stay.
  - Entities that entered play while their script had an error start
    running (`OnCreate`, `OnEnable`, `OnStart`) as soon as a reload
    succeeds, so a fix takes effect without restarting play.
- **`ReloadChanged(changes)`** takes the `HotReloader`'s change list and
  reloads the loaded `.luau` scripts in it. Scripts nobody uses, and
  non-scripts, are skipped. **`DatabaseLoader`** reads script sources
  through the asset database.
- **Error overlay** (`editor/src/ui/error_overlay.h`, portable):
  - A stack of red toasts in the bottom-right corner, such as
    "Mover.luau:12  attempt to index nil".
  - It shows up to N, with a "+N more" line, and a click returns the entry
    so the editor can open the file at that line.

**Verified**: 3 new tests.

- **Reload in play**:
  - A broken edit keeps v1 running, with the error parsed.
  - The fix keeps state, calls `OnReload`, adds new methods, and applies
    new defaults except where an entity overrides them. The next frame
    runs the new code.
- **Through the file watcher**: a script broken from the start of play is
  fixed on disk, and the entity starts running. Unused scripts and
  non-scripts are skipped, and error parsing is checked (including a
  Windows-style path).
- **Headless overlay**: empty, capped with "+N more", and no location.
- 205/205 tests pass on GCC 13, on Clang and under ASan/UBSan, and 212/212
  with physics.

**Step 6, part 1: the debugger core and code completion** (§11.5). These
are the testable engines behind the code editor panel and the DAP server,
which come next.

- **`ScriptDebugger`** (`scripting/include/aether/script/debugger.h`)
  attaches to a `LuauHost`. It is built on Luau's breakpoint instructions
  and single-step callback.
  - **Breakpoints** are set per chunk and line. They can be set before the
    chunk loads, and are re-applied every time it's loaded, so they
    survive hot reload. A line with no code moves to the next line that
    has some, and the line actually used is returned.
  - A line stops once per visit, not once per instruction.
  - **Stepping**: Continue, Step Into, Step Over (calls run through),
    Step Out, and `RequestPause` for the editor's Pause button.
  - **Each stop** reports its frames, innermost first: function, chunk,
    line, and named locals (temporaries left out).
  - **Values are printed without running script code**, so a table's
    `__tostring` can't loop or raise inside the debugger. Strings are
    quoted and cut to 80 characters, vectors print as `(x, y, z)`, and
    tables show their length.
  - The handler runs synchronously on the script's thread. The editor will
    run a nested UI loop there, and the DAP server will block on its
    client.
  - `LuauHost` now remembers each chunk's latest function (for late
    breakpoints). With `allow_debug` it compiles with full debug info, so
    locals have names.
- **`CompleteScript(source, cursor)`**
  (`scripting/include/aether/script/completion.h`) reads the text around
  the cursor. There's no type checker. It offers:
  - The engine API (`world:`, `Input.`, `Timer.`, `Event.`) and the Luau
    libraries (`math.`, `string.`, `vector.`, ...), with signatures.
  - **Component names from reflection** inside `:Get("`, `:Add("`,
    `:Has("`, `:Remove("` and `EntitiesWith("`.
  - A component's reflected fields after `.` (with their types) and its
    functions after `:` (with signatures). This works inline
    (`e:Get("Health").`) and through locals holding entities or
    components. It goes one level into struct and vector fields
    (`t.position.x`).
  - Events, connections and timers (`:Connect`, `:Disconnect`, `:Cancel`).
  - `self.` lists the class's fields, `self.entity` and fields the code
    sets. `self:` lists its methods. `function Class:` lists the lifecycle
    and physics callbacks.
  - Otherwise, keywords, globals and the locals, parameters and loop
    variables declared above the cursor.
  - Prefix matching ignores case. Nothing is offered in comments, strings,
    numbers, or names being declared.

**Verified**: 7 new tests.

- **Debugger**:
  - A breakpoint set before loading stops once per call, with the right
    locals in both frames.
  - Late breakpoints on blank lines move to code, one past the end is
    refused, and breakpoints are re-applied after a reload.
  - Into, Over and Out take the expected path, and Pause stops.
  - A table whose `__tostring` errors is printed safely.
- **Completion**: modules, locals and loop variables, case-insensitive
  prefixes, and the replace range. Also tested: no completion in
  comments, strings or numbers; component names; fields and methods with
  types, through locals and inline calls, and nested; events, timers and
  connections; and `self` and callbacks.
- 212/212 tests pass on GCC 13, on Clang and under ASan/UBSan, and 219/219
  with physics.

**Step 6, part 2: the DAP server** (`scripting/include/aether/script/dap.h`).
VS Code and other Debug Adapter Protocol clients can debug the game's
scripts through the `ScriptDebugger`. It doesn't depend on a transport: the
editor passes in the bytes it receives and sends what the session writes.
The socket listener comes with the Windows editor hookup.

- **`DapFraming`** handles DAP's `Content-Length` framing.
  - It reassembles messages that arrive in pieces or several per read.
    Lengths are counted in bytes, so UTF-8 text is safe.
  - A message that isn't JSON is skipped and counted. A header without a
    length drops the buffer, since the next message can't be found.
- **`DapSession`** handles these requests: `initialize` (then sends the
  `initialized` event), `launch`/`attach`, `configurationDone`,
  `setBreakpoints`, `threads`, `stackTrace`, `scopes`, `variables`,
  `evaluate`, `continue`, `next`, `stepIn`, `stepOut`, `pause` and
  `disconnect`. Other requests get an error response.
  - Scripts appear as one thread, "Scripts".
  - **Breakpoints**: a source path maps to its chunk (by default, the file
    name). A breakpoint in a script that hasn't loaded yet is accepted but
    shown unverified until it loads. One past the end of the file is
    refused with a message.
  - **At a stop** it sends `stopped` (reason `breakpoint`, `step` or
    `pause`) and blocks inside the debugger's handler, reading requests
    until one resumes execution.
  - **Inspecting**: each stack frame has a Locals scope. `evaluate`
    returns the value of a local in the chosen frame, which covers hover
    tooltips; other expressions are refused.
  - **Pause while running** stops at the next line a script runs.
  - **If the client disconnects**, or sends `disconnect`, its breakpoints
    are cleared and the game runs on.
  - Stops before `initialize` don't block.

**Verified**: 3 new tests.

- **Framing**: split and joined reads, UTF-8, invalid JSON skipped, and
  a header without a length.
- **A scripted client**:
  - A breakpoint set before load is unverified; re-sent after load it's
    verified on line 3, and one past the end is refused.
  - Paths come back in stack frames. Locals, `evaluate` (including in the
    caller's frame) and Step Out work.
  - Breakpoints cleared from inside a stop take effect.
- **Edge cases**: pause while running, unknown requests, stepping while
  running, the client vanishing mid-stop, and `disconnect`.
- 215/215 tests pass on GCC 13, on Clang and under ASan/UBSan, and 222/222
  with physics.

**Step 6, part 3: the code editor panel** (`editor/src/ui/code_editor.h`,
portable). This finishes Phase 11 on the portable side. Hooking the panel,
completion, the debugger and a DAP socket into the Windows editor window is
Windows editor work.

- **`CodeDocument`** is the text model and has no UI.
  - **Text**: lines of UTF-8, and the cursor never lands inside a code
    point. CRLF and tabs are normalized on load. Offsets convert both ways,
    for completion.
  - **Moving**: arrows, words, Home (to the first non-blank character, then
    to column 0), End, and selection.
  - **Editing**:
    - Enter auto-indents after `then`, `do`, `else`, `repeat`, `function(...)`,
      `{` and `(`, ignoring comments.
    - Typing `end`, `else`, `elseif`, `until` or `}` alone on a line takes
      back one level.
    - Backspace in the indentation goes back an indent stop.
    - Tab and Shift+Tab indent or unindent the selected lines, and Ctrl+/
      toggles `-- ` at their common indent.
  - **Undo/redo** keeps snapshots. A run of typing is one step, and moving
    or punctuation ends it. `Dirty()` is false again after undoing back to
    the saved text.
  - **Find** is forward or backward, wraps, and can match case.
  - **`TokenizeLuauLine`** highlights keywords, the engine's builtins
    (`self`, `world`, `Input`, `math`, ...), numbers (`2.5e-3`), strings
    with escapes, and comments. Long comments and strings (`--[[`,
    `[==[`) carry across lines. A field named like a keyword (`t.end`)
    stays plain text.
- **`DrawCodeEditor`** is the ImGui widget, in a scrolling child window.
  - **Gutter**: line numbers and breakpoint dots; clicking toggles a
    breakpoint and reports it.
  - **Markers**: the debugger's current line (a yellow arrow and band) and
    the error line (a red band with an underline, and the message on
    hover).
  - **Text**: highlighted, with the selection and cursor drawn.
  - **Mouse**: click to place the cursor, Shift+click or drag to select.
    Clicking outside drops focus.
  - **Keys**: everything the model does, plus PageUp/PageDown, Ctrl+A/C/X/V,
    Ctrl+Z/Y, Ctrl+S (reported as a save request) and read-only mode.
    While it's focused, it owns Tab, Enter and the arrows, so they edit
    instead of moving focus.
  - **Completion popup**: it opens while typing a name or after `.`/`:`,
    and on Ctrl+Space. Up/Down pick, Enter or Tab accept (one undo step),
    and Escape or a non-name character closes it.
    - It takes its items from a callback, which the editor wires to
      `script::CompleteScript`, so this library doesn't depend on
      scripting.

**Verified**: 6 new tests.

- **The model**:
  - Offsets, clamping, moving and UTF-8.
  - Auto-indent of a whole function typed key by key, Enter mid-line,
    comments, `else`, Backspace stops, Tab and Shift+Tab with a selection,
    and comment toggling.
  - Undo runs, dirty tracking through undo and save, and find with
    wrapping and case.
  - Tokens for numbers, strings, members, and long comments and strings
    across lines.
- **The widget, headless**:
  - Keys are ignored until it's focused.
  - Typing and auto-indent, selection, select all, copy, undo/redo, save,
    Tab staying in the editor, Ctrl+/, and read-only.
  - Clicking and Shift+click selection, breakpoint toggling, and the
    completion popup: open, refine, pick with the arrows, accept, undo,
    Escape and Ctrl+Space.
  - Losing focus on an outside click.
- 221/221 tests pass on GCC 13, on Clang and under ASan/UBSan, and 228/228
  with physics.

### Phase 12 (done on the portable side) — Blueprints (visual scripting)

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md),
Phase 12. The node reference is
[`docs/design/BLUEPRINT_NODES.md`](docs/design/BLUEPRINT_NODES.md).

**Step 1: the graph model, node signatures and validation** (§12.2). The
new `Aether::Blueprint` library (`blueprint/`) depends only on the engine.

- **Types** (`types.h`):
  - Pin types: exec, bool, int, float, string, Vec3, Quat, Entity, a
    reflected struct, Wildcard, and `Array<T>`, each with a stable name.
  - Literal values with JSON.
  - `CanConnect` says whether a link works as is, needs a conversion
    (int→float, or anything→string), or loses data (float→int, which
    gets warning BP104).
- **Graph data** (`graph.h`):
  - Nodes, links, graphs (Event Graph, Function, Macro) and variables
    (instance-editable, expose-on-spawn, tooltip, category).
  - `.abp` JSON in the §A.4 shape; a round trip gives the same document.
    Loading refuses newer formats, duplicate node IDs, unknown types and
    malformed links, each with a message.
  - `GraphBuilder` builds graphs in code.
- **Node types** (`nodes.h`). Pins are never saved: `ResolveNode` works them
  out from the node's type ID, its config and the Blueprint. So a
  variable's type change, a Sequence's output count or a function's
  signature can't leave stale pins in the file.
  - **Built in**:
    - events: BeginPlay, EndPlay, Tick, FixedTick, trigger and collision,
      and Custom Events with parameters, plus their `Call.Custom:` nodes
    - Branch and Sequence
    - `Var.Get:`/`Var.Set:` per variable
    - literals, and typed math families (`Math.Add:float`, comparisons,
      boolean logic, clamp and lerp)
    - Vec3 nodes, Get Self, Is Valid and Print String
    - Function Entry/Return and `Call.Self:`
  - **From reflection**:
    - `Call.Native:Type.Fn` for every `BlueprintCallable` function;
      `Pure` ones have no exec pins, and members get a `target` entity
      that defaults to self.
    - `Comp.Get:`/`Comp.Set:` for every `BlueprintReadWrite` field.
  - **Custom node types** register through `RegisterNodeType` and
    `RegisterNodeFamily`, the same way the built-ins do.
  - **`ListNodeTypes`** is the palette: every node this Blueprint can use,
    sorted by category.
- **Validation** (`validate.h`) reports errors and warnings from the
  catalog, each on its node and pin. The messages are the §13 wording.
  - Errors:
    - BP001 type mismatch
    - BP003 pure loops (naming the chain)
    - BP004 a function that no longer exists
    - BP005 a deleted variable
    - BP006 a duplicate event (custom events by name)
    - new codes: BP007 unknown type or bad config, BP008 bad pin,
      direction, default or node, BP009 too many links, BP010 an event
      outside the Event Graph, BP011 a function's entry count
  - Warnings: BP101 Branch's condition left unconnected (with the default
    used), and BP104.

**Verified**: 5 new tests.

- Type names, parsing, conversions and values.
- BP_Door from §A.4: it loads, round-trips through JSON and disk, and
  validates clean, with the right pins. Load errors are checked too.
- Signatures:
  - from config: Sequence outputs, custom events and their calls, and
    math operand types
  - from reflection: callable, pure and hidden functions, and fields
  - function graphs, a registered custom type, and the palette
- Every validation code on its node, pin and severity, plus function
  graph rules and allowed implicit conversions.
- 226/226 tests pass on GCC 13, on Clang and under ASan/UBSan, and 233/233
  with physics.

**Step 2: the compiler and VM** (§12.4, ROADMAP_DETAILS §C).

- **`CompileBlueprint`** validates first, then lowers every event and
  function graph to typed register bytecode (`bytecode.h`).
  - **Registers**: two banks per frame, 16-byte value registers (bool,
    int, float, Vec3, Quat, Entity) and string registers.
  - **Typed opcodes**: `ADD_F`, `ADD_V3`, `CMP_LT_I`, `CLAMP_F`, and so
    on. The VM never checks a type in its hot loop.
  - **Exec flow**:
    - Links are walked from each entry: Branch becomes `JMPF`, and a
      Sequence runs its outputs in order.
    - An exec wire back into a node already on the path becomes a jump,
      so loops are guarded by the instruction budget.
  - **Pure nodes** are emitted right before the impure node that reads
    them, and cached for that node only. So `count = count + 1` twice
    reads `count` again the second time.
  - **Implicit conversions** become `CONV_I2F`, `CONV_F2I` and `TOSTR`.
  - **Calls**:
    - Reflected functions and component fields go through
      `FunctionInfo` and `FieldInfo`.
    - Blueprint functions (pure or not, with early Return nodes) and
      custom events get their own frame.
  - **Unsupported types**: structs and arrays are refused for now with
    the new BP012.
  - **`Disassemble`** prints a function as text, for golden tests and
    debugging.
- **`BlueprintVM`** runs instances attached to entities, each with its
  own variables.
  - Instance-editable overrides are applied at attach time.
  - **Running events**: `Dispatch` runs any event with arguments.
    `BeginPlay` and `Tick` run on every instance, in attach order.
  - Variables can be read and set from C++, and Print String goes to a
    handler.
  - **Frames** are reused per call depth, so a dispatch allocates nothing.
  - Instances can be attached or detached from inside a running event.
- **Runtime safety** (BLUEPRINT_NODES.md §13):
  - BP202 stops an event after its instruction budget (1M by default) and
    names the node. The budget is per dispatch.
  - BP201: a call or field access on an entity without the component is
    logged once per node. It returns a default value, and execution goes
    on.
  - The new BP203 limits call depth (endless recursion), and BP204
    reports a refused native call.
  - Division by zero gives 0.

**Verified**: 5 new tests.

- **BP_Door**:
  - The golden bytecode for its trigger event and `Open` function.
  - Opening once and staying open, missing events and non-instances,
    overrides per instance, and detaching.
- **Math and flow**:
  - int→float→string, Sequence, and per-step caching of pure nodes.
  - Branch, Vec3 printing, string equality, and divide by zero.
  - float→int truncation, and Self/Is Valid.
  - Variables from C++, and separate instances.
- **Engine hookups**:
  - Tick accumulating delta time, and instances without Tick skipped.
  - Native calls (impure and pure) and component Get/Set.
  - BP201 once with a default result.
- **Functions**: pure and impure functions and custom events, called from
  a graph and from C++.
- **Failures**:
  - BP202 on an exec loop, stopped at the right node and fresh on the
    next dispatch.
  - BP203 on recursion.
  - BP012 and validation errors blocking the compile.
- 231/231 tests pass on GCC 13, on Clang and under ASan/UBSan, and 238/238
  with physics.

**Step 3: latent actions, stateful flow nodes and the lifecycle** (§12.4
point 3, ROADMAP_DETAILS §C.4).

- **Latent nodes** (BLUEPRINT_NODES.md §3):
  - Delay: calling it again while it waits is ignored.
  - Retriggerable Delay: calling it again restarts the timer, keeping the
    newer values.
  - Delay Until Next Tick.
  - How they run:
    - The node starts a latent action and execution goes straight on,
      so a Sequence runs its next output immediately.
    - Its "completed" wire runs on a later frame, from a saved copy of
      the event's registers, so event parameters survive the wait.
    - A wire back into the Delay makes a repeating timer.
  - Latent nodes in functions are refused with BP002.
- **Stateful flow nodes** (§2): Do Once (with reset and start closed),
  Do N (with its counter), Gate (enter, open, close, toggle, start closed)
  and Flip Flop (A/B, with `is_a`).
  - Their state is per instance.
  - It's shared by all events of the graph, so one custom event can open
    a Gate that another passes through.
- **`BlueprintVM::Tick(dt)`** advances game time and resumes due actions
  in wake order, then runs Event Tick.
  - Disabled instances neither tick nor resume, so their timers pause.
  - A detached or destroyed owner's actions are dropped.
- **`BlueprintInstance`** is the component: a Blueprint asset reference
  plus instance-editable overrides. The engine also gets a new
  `BlueprintAsset` tag.
- **`BlueprintSystem`** runs instances through the Phase 9 lifecycle.
  - **Entering play**: the Blueprint is loaded and compiled once per
    asset, and the entity attached with its overrides.
  - **Start** runs BeginPlay. **Enable/disable** resumes or pauses the
    instance.
  - **Destroy or end of play** runs EndPlay, then detaches it.
  - **Ticking**: `Update(dt)` ticks everything once per frame.
  - **Compile failures** are reported once per asset in
    `CompileErrors()`, and those entities don't run.
- **Fix**: literal pin defaults such as Delay's 0.2 s, Clamp's max of 1
  and Do N's n of 1 were being taken as pin flags by an overload, and
  came out as 0. Clang's `-Wliteral-conversion` caught it. The flags
  parameter is now a `PinFlags`, and a regression test checks the
  defaults.

**Verified**: 5 new tests, plus the defaults regression check.

- **Delay**:
  - A Sequence carrying on past it, and a second call ignored while
    waiting.
  - Resuming at the right time, with the event's parameter preserved.
- **Retrigger, next tick and loops**:
  - Retriggerable Delay restarting with the newer value.
  - Next tick resuming before Event Tick.
  - A Delay loop counting 4 ticks.
- **Stateful nodes**:
  - Do Once, Do N, Gate and Flip Flop sequences.
  - State per instance, and Do Once starting closed.
- **Owners and rules**:
  - Pause while disabled, and actions dropped on detach and destroy.
  - BP002 on its node.
- **Lifecycle**:
  - Overrides, compiled once per asset, and broken Blueprints reported
    once and not run.
  - Tick counts, and a disabled entity not ticking.
  - EndPlay on destroy and at the end of play.
- 236/236 tests pass on GCC 13, on Clang and under ASan/UBSan, and 243/243
  with physics.

**Step 4, part 1: loops, switches, strings and more math** (BLUEPRINT_NODES.md
§2, §5, §8, §9).

- **Loops**:
  - For Loop runs from first to last inclusive. An empty range runs no
    body, but `completed` still fires.
  - For Loop with Break: `break` is usually wired from inside the body,
    and stops the loop after the current iteration.
  - While Loop re-reads its condition every iteration.
  - All compile to plain jumps, so they cost no calls. They run within
    the event's instruction budget.
- **Switches and Select**:
  - Switch on Int and Switch on String have their cases in the node's
    config, plus `default`. Duplicate cases, or a case named `default`,
    are refused with BP007.
  - Select picks one of 2 to 16 options of any type. An out-of-range
    index gives the type's default.
- **Strings**: Append (2 to 16 inputs), Length (in bytes), Is Empty,
  Contains (optionally ignoring case), To Upper/Lower and Trim.
  - String to Int and String to Float have a `success` pin, and reject
    trailing junk such as `42abc`.
  - **Format Text** gets one input pin per `{name}` placeholder, and
    `{{`/`}}` are literal braces. Anything connected to a placeholder is
    converted to text (numbers, vectors, entities).
- **Conversions**: `Conv.ToString:<type>` for bool, int, float, Vec3, Quat
  and Entity.
- **Math**:
  - Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sqrt, Exp, Log, Power and
    Frac, plus degrees/radians conversion.
  - Floor, Ceil, Round and Truncate return an int.
  - Negate, Abs and Clamp for ints, Nearly Equal with a tolerance, and Map
    Range Clamped.
  - Random Float/Int in Range and Random Bool, seeded per VM
    (`Options::random_seed`, so tests are reproducible).
  - Math can't produce NaN or infinity: sqrt and log of negatives, and a
    `pow` overflow, give 0.

**Verified**: 4 new tests.

- **Loops**: For Loop, the break at index 2, and a While loop counting
  to 3.
- **Switches and Select**: every case and the default, Select in range
  and out of range, and bad configs refused with BP007.
- **Strings**: all the string nodes and conversions, including Format
  Text with a number, a vector and escaped braces, and byte length for
  UTF-8.
- **Math**:
  - All the math nodes, including rounding direction and the safe
    sqrt of a negative.
  - Random values within range and identical for the same seed.
- 240/240 tests pass on GCC 13, on Clang and under ASan/UBSan, and 247/247
  with physics.

**Step 4, part 2: arrays and For Each** (BLUEPRINT_NODES.md §9, §2).

- **Array values** have their own register bank, and there are array
  variables per instance (`Array<int>`, `Array<string>`,
  `Array<Entity>`, ...).
  - Frames, function calls and latent resumes carry them like other
    values.
  - From C++, `GetArray` and `SetArray` read and write them.
- **Pure nodes**: Make Array (0 to 16 items), Length, Last Index, Get (a
  copy), Is Valid Index, Find (-1 if absent) and Contains.
- **Changing an array in place**: Add, Add Unique (-1 if already
  present), Insert (the index is clamped), Remove Index, Remove Item,
  Clear, Set Array Elem and Reverse.
  - Like Unreal's by-reference pins, their `array` input must come from a
    Get node of an array variable. Anything else, or leaving it
    unconnected, is the new BP013, on that pin.
- **For Each** iterates a copy with `element` and `index`, so changing
  the array in the body doesn't change the loop.
- **Out of range**:
  - Get returns the element type's default (a null entity for entities)
    and logs the new BP205 once per node. Set Array Elem does nothing and
    logs BP205.
  - Remove Index does nothing, silently.
  - Elements compare by value, including strings (Find, Contains, Add
    Unique, Remove Item).

**Verified**: 3 new tests.

- **Changing arrays**: in-place changes to int and string array
  variables through every modifying node, the query nodes, and
  `GetArray`/`SetArray`.
- **Make and For Each**:
  - A Make → Set → For Each sum, where the body appends to the same
    array but still runs 3 times.
  - An out-of-range Get giving 0 with one BP205 on the right node, and an
    entity array defaulting to "no entity".
- **Errors**: BP013 for an Add fed by Make Array and for an unconnected
  pin, BP007 for bad element types and item counts, and BP001 between
  array types.
- 243/243 tests pass on GCC 13, on Clang and under ASan/UBSan, and 250/250
  with physics.

**Step 4, part 3: entity and world nodes** (BLUEPRINT_NODES.md §6).

- **Transforms**:
  - Get/Set Location and Rotation, and Add Offset, all relative to the
    parent.
  - Get World Location, through the parent chain.
  - Rotate Vector, Rotation from Axis and Angle (degrees), and Combine
    Rotations.
  - An entity that's gone, or has no Transform, gives defaults and one
    BP201 per node.
- **Tags**: Has/Add/Remove Tag and Find Entities with Tag, which returns
  an `Array<Entity>`.
- **Hierarchy**: Get Parent, Attach To and Detach. They need
  `SetGuidIndex`, which `BlueprintSystem` sets from the lifecycle. Attach To
  refuses to make an entity its own ancestor (BP201).
- **Lifetime**:
  - **Destroy Entity** is deferred until the running event finishes, so
    the nodes after it still run. Then EndPlay runs, the instance is
    detached, and the entity is destroyed, through the lifecycle when
    there is one.
  - **Spawn Blueprint** takes the Blueprint asset from the node's config,
    plus a location and rotation, and returns the spawned entity.
    - It goes through a spawner hook. `BlueprintSystem` creates the entity
      with an ID, a Transform and a `BlueprintInstance`, and attaches it at
      once, so the spawner can use it straight away.
    - The lifecycle runs its BeginPlay at the next sync.
    - Without a spawner, the node gives no entity and the new BP206.
- **Time**: Get Game Time and Get Delta Seconds.
- **Robustness**:
  - Instances whose entity was destroyed behind the VM's back are
    dropped at the next Tick.
  - An entity spawned and destroyed before the lifecycle saw it still
    gets exactly one EndPlay.
  - The engine's `Lifecycle` gets a `Guids()` accessor.

**Verified**: 4 new tests.

- **Transforms**: set, offset and read back, a 90° turn giving forward
  (1, 0, 0), and BP201 for an entity without a Transform.
- **Tags, hierarchy and time**:
  - Tags on two entities, counted and removed.
  - Attach/detach with the world location through the parent, and a
    refused cycle.
  - Game time and delta seconds.
- **Destroy and spawn**:
  - Destroy running the rest of the event before EndPlay.
  - Spawn with and without a spawner (BP206).
- **Through the lifecycle**: a gun Blueprint spawning a bullet Blueprint
  through `BlueprintSystem`. The bullet starts at the next sync and is
  destroyed with exactly one EndPlay.
- 247/247 tests pass on GCC 13, on Clang and under ASan/UBSan, and 254/254
  with physics.

**Step 4, part 4: event dispatchers and interfaces** (BLUEPRINT_NODES.md
§10, §8, ROADMAP §12.1). Blueprints can now talk to each other.

- **Event dispatchers** are declared on a Blueprint (`OnOpened(by:
  Entity)`) and saved in the .abp.
  - **Call \<Dispatcher\>** runs every Custom Event bound to the target
    entity's dispatcher, with the call's arguments.
  - **Bind / Unbind / Unbind All** name a Custom Event handler of the
    listener's own Blueprint. A listener can bind to another class's
    dispatcher, since binding goes by name. If the listener declares a
    dispatcher of that name, the handler's parameters must match it (the
    new BP014).
  - **Bindings**:
    - Binding twice is one binding, and handlers may bind or unbind
      while the dispatcher is running.
    - A binding goes away when either side is detached or destroyed.
- **Blueprint Interfaces** are registered with
  `RegisterBlueprintInterface` (an interface name and its functions'
  parameters).
  - A Blueprint lists the interfaces it implements, and handles
    `Event.Interface:Interactable.Interact`.
  - A Blueprint that doesn't list the interface can't have its events
    (the new BP015).
  - **Interact (Message)** calls the target's implementation. On an
    entity that doesn't implement it, it does nothing, with no error,
    like Unreal.
  - **Does Implement** answers whether it does.
  - The palette offers the interface's events to implementers.

**Verified**: 3 new tests.

- **Dispatchers**:
  - Two listeners bound to a door and called with the instigator.
  - Duplicate binds, Unbind, a detached listener unbound, Unbind All,
    and the .abp round trip.
- **Dispatcher errors**: BP014 for mismatched parameters, and BP004 for
  a missing handler or an undeclared dispatcher.
- **Interfaces**:
  - A lever implementing Interactable, called from a player.
  - Non-implementers and non-instances ignored silently.
  - BP015, BP004, the palette, and the .abp round trip.
- 250/250 tests pass on GCC 13, on Clang and under ASan/UBSan, and 257/257
  with physics.

**Step 4, part 5: macros** (BLUEPRINT_NODES.md §12).

- **Macro graphs** have an Inputs and an Outputs node (tunnels), and a
  signature that can include exec pins. So a macro can have several exec
  inputs and outputs, like Unreal's Gate-style macros. Macro graphs are
  saved in the .abp like functions.
- **A `Macro:Name` node** uses a macro. Its pins are the macro's
  signature. It is pure if the macro has no exec pins.
- **Inlining**: the compiler replaces every instance with a copy of the
  macro's nodes, rewired through the tunnels, before compiling.
  - Macros inside macros are expanded too, and values can pass straight
    through.
  - Each copy has its own node IDs, so a Do Once or Delay inside a macro
    has separate state per instance (as in Unreal).
  - Latent nodes are allowed in macros used from event graphs.
  - An unconnected macro input feeds its value (the instance's default,
    else the macro's) through a literal node, so conversions apply as
    usual (an int macro input printed as text, for example).
- **Errors**:
  - Macros that contain themselves, directly or through others, are
    refused with the new BP016.
  - The expanded graphs are validated again. A problem that only appears
    once inlined, such as a Delay macro used in a function (BP002), is
    reported on the instance node as "Inside the macro: ...".

**Verified**: 3 new tests.

- **Inlining**: an exec macro running its output twice, and a pure macro
  called with a link, with the instance default and with the macro
  default. Also a macro nested in a macro, and a pass-through value into
  a string pin.
- **State and latent nodes**: a Do Once macro used twice with separate
  state and reset, and a Delay macro resuming later.
- **Errors**: BP016 for mutual recursion, BP004 and BP010, and BP002
  from an inlined latent node, reported on the function's instance node.
- 253/253 tests pass on GCC 13, on Clang and under ASan/UBSan, and 260/260
  with physics.

**Step 4, part 6: Sort, Filter and Expose on Spawn**. This completes
step 4, the v1 node library.

- **Sort** changes an array variable in place.
  - It uses the natural order for ints, floats and strings, or a
    comparator function of the Blueprint (config `by`: `(a, b) -> bool`,
    "a comes before b").
  - It's a stable merge sort written by hand, so an inconsistent
    comparator scrambles the order but can't crash (unlike `std::sort`).
  - A comparator that fails, such as one that runs out of budget, stops
    the event.
- **Filter** returns the items a predicate `(item) -> bool` accepts, and
  is pure.
- **Errors**:
  - A comparator or predicate with the wrong signature is refused with
    the new BP017.
  - A missing one is BP004.
  - Sorting Vec3s (no natural order) without one is BP007.
- **Expose on Spawn**:
  - The Spawn Blueprint node gets one input pin per variable listed in
    its config `expose` (the editor fills it in from the class).
  - The values reach the spawner as JSON, and `BlueprintSystem` applies
    them when it attaches the new instance, before its BeginPlay.
  - Only variables marked Expose on Spawn (or Instance Editable) accept
    them.
  - The spawner hook now gets these values as a third argument.

**Verified**: 3 new tests.

- **Sorting and filtering**: ints ascending, descending by a Blueprint
  comparator, strings, and a filter of the even numbers. A 100-item sort
  matches `std::sort`.
- **Sort errors**: BP007, BP017 and BP004, each on its node.
- **Spawning**: a bank spawning a coin with value 25 and label "gold",
  printed by the coin's BeginPlay. A non-exposed variable is ignored, and
  a bad exposed pin is refused.
- 256/256 tests pass on GCC 13, on Clang and under ASan/UBSan, and 263/263
  with physics.

**Step 5: the Blueprint debugger** (§12.5).

- **Breakpoints on nodes** (`SetBreakpoint(blueprint, graph, node)`)
  work in event graphs and function graphs.
  - A breakpoint in a loop body stops every iteration.
  - Setting one on a node with no code is refused.
- **At a stop**, the handler gets:
  - the reason (breakpoint, step or pause);
  - the call stack, innermost first, each frame with its function, graph
    and current node;
  - every pin's current value, for data-pin hovers and watches: "42",
    "true", "(1, 0, 0)", "Entity(3v1)", "[4 items]".
- **Actions**: Continue, Step Into (enters called functions), Step Over
  (runs them) and Step Out (back to the caller's next node).
  `RequestPause` is the Pause button: it stops at the next node any
  Blueprint runs.
- **Instance filter**: "Debug: BP_Door_2" only stops for that entity.
- **Exec trace** for wire animation: every node that starts running is
  recorded (entity, graph, node, frame number) in a capped buffer. The
  editor takes it each frame, and it works without a debug handler.
- **Cost when not debugging**: one flag test per instruction. The
  compiler records where each node's code starts, and which register
  holds each pin's value.

**Verified**: 3 new tests.

- **Breakpoints**:
  - One in a called function stopping with two frames, and the caller's
    pin values visible (40 + 2 = "42").
  - Then the event's Print with the call's result ("84").
  - Clearing and detaching, and a loop-body breakpoint stopping three
    times with the index.
- **Stepping**:
  - Step Over not entering the function.
  - Step Into entering it and Step Out returning to the next node.
  - Pause, and the stop count.
- **Filter and trace**:
  - The instance filter.
  - The trace listing nodes in execution order across the function
    call, with the frame number, and the trace capped to its latest
    entries.
- 259/259 tests pass on GCC 13, on Clang and under ASan/UBSan, and 266/266
  with physics.

**Step 6, part 1: the graph editor widget** (§12.6). Portable ImGui code in
`aether_editor_ui`, tested headless.

- **The widget** (`editor/src/graph/graph_view.h`) is generic, so the
  Material and Animation editors can reuse it. It draws a view model and
  reports what the user asked for; the owner applies it.
  - Mouse: drag nodes (Ctrl+click adds to the selection), drag between
    pins to link, drop a wire on empty canvas or right-click for the
    palette, drag on empty canvas to box-select, Alt+click a wire to
    break it, middle- or right-drag to pan, wheel to zoom about the
    cursor.
  - Keys while hovered: Delete, Home (fit everything) and Ctrl+A.
  - Drawing: exec pins as triangles, arrays as grids, filled when
    connected; bezier wires; compile errors as red outlines; the
    debugger's node highlighted; breakpoint dots; comment bubbles.
- **The Blueprint adapter** (`blueprint_graph.h`):
  - builds the view: pin colors from BLUEPRINT_NODES.md, header colors
    for events, latent nodes, macros, flow, pure and call nodes, errors
    from validation, and wires glowing from the exec trace;
  - applies edits: links checked with `CanConnect` (direction, kind,
    type), and a new link to a data input or exec output replaces the
    old one, as in Unreal;
  - searches the palette: fuzzy, prefix matches first, and only nodes
    with a pin fitting the dragged one.

**Verified**: 4 new tests.

- The view's colors, labels and debug state.
- Edits refusing bad links and replacing old ones, deletes taking their
  links, and a node placed from a wire linking its first fitting pin.
- Palette search ranking and pin-type filtering.
- The widget driven by simulated input: node drag, wire link, palette
  from a dropped wire and from right-click, Alt+click break, box select
  and Delete, zoom keeping the point under the cursor, and fit.
- 263/263 tests pass on GCC 13, on Clang and under ASan/UBSan, and 270/270
  with physics.

**Step 6, part 2: the Blueprint editor panels** (§12.6). This completes
step 6. Portable code in `aether_editor_ui`, tested headless.

- **The document** (`editor/src/graph/blueprint_document.h`) holds the
  open Blueprint and its file, with no ImGui.
  - Undo/redo keeps whole-Blueprint snapshots. Typing in one field is one
    step.
  - It tracks the compile status: not compiled, OK, warnings, errors, or
    changed since.
  - My Blueprint edits: add, rename and remove variables, functions,
    macros and event dispatchers. A rename updates the nodes that refer
    to it; a removal takes them away. Changing a variable's type breaks
    the links that no longer fit.
  - Copy, cut, paste and duplicate nodes, with the links between them.
- **Comment boxes** are saved in the graph (`comments` in the `.abp`).
  Press C to box the selection. Dragging a box's title moves it and the
  nodes inside, and its corner resizes it.
- **The editor** (`blueprint_editor.h`) lays out the panels from
  ROADMAP.md §12.6:
  - a toolbar: Compile (colored by status), Save, Undo/Redo, and the
    parent class;
  - My Blueprint: graphs, functions, macros, variables (drag one onto the
    graph for a Get node), event dispatchers and interfaces;
  - graph tabs, opened by double-clicking a graph or a call node;
  - Details: a variable's name, type, default and flags; a function's or
    macro's inputs and outputs; a dispatcher's parameters; a node's
    comment and pin defaults; a comment box's text and color;
  - Compiler Results, where clicking a row selects and frames its node;
  - the node palette, searchable, filtered by a dragged pin, and closed
    with Esc.
- **Keys**: Ctrl+Z/Y (undo/redo), Ctrl+S (save), F7 (compile), and in the
  graph Tab (palette), F (frame the selection), C (comment) and
  Ctrl+C/X/V/D.
- **Not yet**: the Class Defaults and Components tabs, the diff view, the
  minimap and bookmarks, reroute nodes, and collapsing a selection into a
  function.

**Verified**: 5 new tests.

- Undo/redo with merged edits, save and load, and the compile status.
- Renames and removals following references, name checks, and a type
  change breaking only the links that no longer fit.
- Paste positions and links, junk clipboards refused without an undo
  step, duplicate, and comment boxes round-tripping through the `.abp`.
- The widget moving a comment box with the node inside it, resizing it,
  and deleting it.
- Every Details page drawing, tabs following a rename, a Compiler Results
  row focusing its node, the palette from a pin (filtered, placed and
  linked, closed with Esc), and the keys driven by simulated input.
- 268/268 tests pass on GCC 13, on Clang and under ASan/UBSan, and 275/275
  with physics.

**Step 7: samples and the 10,000-instance benchmark**. This completes
Phase 12 on the portable side.

- **Samples** in `assets/blueprints/`, saved exactly as the editor saves
  them, with comment boxes:
  - `BP_Door` opens while an entity tagged Player stands in its trigger:
    it swings at `Speed` degrees per second up to `OpenAngle` (both
    instance-editable), and closes when the player leaves.
  - `BP_Coin` calls its `OnCollected(value)` event dispatcher when the
    player touches it (once), then destroys itself.
  - `BP_GameMode` binds its `AddScore` event to every Coin at BeginPlay,
    prints "Score: N", and prints "You win" once when the score reaches
    `Goal` (10).
  - `BP_Spinner` is the benchmark's 20-node Tick graph: spin, bob, and
    count laps.
- **Benchmark**: `aether_bp_bench [--count N] [--frames N] [--check MS]`
  times `BlueprintVM::Tick` on BP_Spinner. 10,000 instances take about
  1.4 ms per frame here; the target is under 2 ms.
  - The first run took about a second per frame. After every dispatch
    the VM scanned all instances for ones detached while running, so
    cost grew with the square of the count. It now keeps a list of
    those instances.
  - Tick also calls each instance's Tick function directly, without
    building a string to look it up.
- **Not yet**: no Timeline node, so the door eases in Tick. Trigger
  events come from the tests until Phase 13 sends physics contacts.

**Verified**: 4 new tests.

- Every sample validates with no errors or warnings, compiles, and
  round-trips to the same JSON.
- The door ignores a crate, opens for the player (45 degrees after half
  a second, stopping at 90, with the matching rotation), and closes back
  to 0.
- Coins ignore a crate. Nine coins score 9 and disappear; the tenth
  prints "You win", once. A coin worth 5 (an override) makes it 15.
- 10,000 spinners tick 60 frames, each finishing one lap. Optimized
  builds fail the test above 20 ms per frame, to catch large regressions
  such as the quadratic one.
- 272/272 tests pass on GCC 13, on Clang and under ASan/UBSan, and 279/279
  with physics.

### Phase 13 (done on the portable side) — Physics events and character movement

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md),
Phase 13.

**Step 1: collider components and the motion-only RigidBody** (§13.4).

- **Colliders** (`physics/include/aether/physics/components.h`):
  `BoxCollider`, `SphereCollider`, `CapsuleCollider` (upright, height
  including the caps), `ConvexCollider` (the hull of a point cloud) and
  `MeshCollider` (triangles).
  - Each has a center offset, a trigger flag, friction and restitution.
  - Several on one entity make one compound body.
  - A moving mesh collider uses the convex hull of its vertices, since
    Jolt (like Unity and Unreal) only collides with triangle meshes on
    static bodies.
- **`RigidBody`** now says only how a body moves: Static, Kinematic or
  Dynamic, mass, linear and angular damping, gravity scale, continuous
  collision detection, and rotation locks per axis. Colliders with no
  RigidBody make a static body.
- **Old scenes still load.** The pre-split RigidBody (radius, mass,
  is_static) is read from both binary and JSON scenes, and
  `MigrateLegacyRigidBodies` turns its radius into a SphereCollider.
- **`PhysicsScene`** (`physics_scene.h`) keeps Jolt bodies in step with
  the ECS:
  - it creates a body for each entity with colliders, rebuilds it when a
    collider or the RigidBody changes, and removes it with the entity or
    its last collider;
  - each step, it moves kinematic bodies to their Transform, teleports
    others that gameplay moved, simulates, and writes dynamic bodies back
    to their Transforms;
  - it reports why a collider couldn't make a body (a hull of fewer than
    four points, say);
  - each body carries its entity, ready for contact events in step 3.
- **Editor**: the sample spheres are now RigidBody + SphereCollider, and
  loading a scene migrates old ones. This part of `editor/main.cpp`
  hasn't been compiled yet (it builds only on Windows).

**Verified**: 5 new tests.

- The old binary and JSON RigidBody forms load and migrate, once, and
  skip entities that already have a collider.
- Every collider kind and RigidBody option round-trips through the
  binary and JSON scene formats.
- Bodies of each kind settle at the right height on a floor: sphere,
  flat box, upright capsule, convex cube, and a box-and-sphere compound.
  A triangle-mesh floor holds a ball, a trigger lets one fall through,
  and a moving mesh collider rests as its hull.
- A radius edit rebuilds the body where it is, and a Transform set by
  gameplay teleports a dynamic body. Static bodies stay put and a
  kinematic one follows its Transform. A bad hull reports why until it's
  fixed. Removing the collider or the entity removes the body.
- 284/284 tests pass with physics on GCC 13 and under ASan/UBSan, and
  272/272 without physics on GCC 13, Clang and ASan/UBSan.

**Step 2: layers and the collision matrix** (§13.4).

- **The matrix** lives in the project: `ProjectSettings::collision_matrix`
  holds, for each of the 32 layers, the mask of layers it collides with.
  - Everything collides by default, and a project that never changes it
    stores nothing.
  - `SetLayersCollide(project, a, b, false)` turns off a pair both ways.
    For example, "Ghost" passes through "Default", or pickups ignore each
    other.
- **Physics follows it**: `PhysicsWorld::SetCollisionMatrix` applies it,
  and every body sits on its entity's `Layer`. Changing an entity's layer
  moves its body to that layer.
- **Fix**: Debug builds with physics compile again. Jolt's profiling
  builds need a name for each broadphase layer, which the layer glue
  didn't provide.

**Verified**: 2 new tests.

- The matrix is symmetric, it returns to empty when every pair is back
  on, a file where only one side of a pair says "no" still gives a
  symmetric result, and it round-trips through the project's JSON.
- With Ghost set to pass through Default, a ghost ball falls through
  the floor while a player ball lands. Moving the ghost to the Player
  layer makes it land, and a ghost still lands on the player.
- 286/286 tests pass with physics on GCC 13 (RelWithDebInfo and Debug)
  and under ASan/UBSan, and 273/273 without physics on GCC 13, Clang and
  ASan/UBSan.

**Step 3: contact and trigger events** (§13.1).

- **Collected safely**: Jolt reports contacts from its worker threads
  during the step. They go into a fixed-size buffer with one atomic
  counter, with no locks and no allocation. After the step, on the game's
  thread, they're sorted, so the same scene always gives the same events
  in the same order.
- **Events per entity**: `PhysicsScene::SetEventHandler` receives each
  event for both entities involved, with the contact point, a normal
  pointing at the receiver, and the impact speed:
  - `CollisionBegin` / `CollisionEnd` for solid bodies, once per pair even
    when several parts of a compound touch;
  - `CollisionStay` every step while touching, only for colliders with
    `report_stay`;
  - `TriggerEnter` / `TriggerExit` when either body is a trigger.
- **Resting doesn't flicker**: Jolt drops the contacts of sleeping bodies.
  Aether keeps the pair until a body wakes up away from it, so a pile at
  rest doesn't fire End and Begin again.
- **Destroyed entities**: the survivor still gets the End. Events for an
  entity destroyed by an earlier handler in the same batch are skipped.
- **Blueprints**: `PhysicsEventName` gives the event to dispatch
  ("Event.OnTriggerEnter"), and there's a new Event OnCollisionStay node.
- **Fix**: destroying a `PhysicsWorld` right after a step could trip a
  Jolt assertion in Debug builds, about once in 15 runs.
  - The cause: a worker thread marks a Jolt job done, which lets
    `Update` return, and only then drops its reference to the job. The
    job pool could be destroyed in between.
  - The job-system adapter now waits for those last releases before it
    goes away.
  - Jolt's Debug assertions now log what failed instead of stopping at a
    bare breakpoint trap.

**Verified**: 6 new tests.

- A two-sphere compound lands with one Begin per side. Its normals point
  at each receiver, and its impact speed is about 4.4 m/s after a 1 m
  fall. It falls asleep with no End. Lifted away, it ends once, then
  begins again when it lands.
- Stay comes only when asked, at most once per step.
- A ball falling through a trigger enters and then exits, with no
  collision events.
- A destroyed ball's End still reaches the floor, and a handler that
  destroys an entity stops that entity's later events.
- A 40-ball pile falling through a trigger on 4 worker threads gives the
  identical event sequence in 10 runs.
- BP_Coin, collected by a falling player ball through a real trigger,
  destroys itself; a ball that isn't the player doesn't collect it.
- 292/292 tests pass with physics on GCC 13 (RelWithDebInfo, and Debug
  40 runs in a row) and under ASan/UBSan, and 273/273 without physics on
  GCC 13, Clang and ASan/UBSan.

**Step 4: physics queries and their Blueprint nodes** (§13.2).

- **Queries**: `PhysicsWorld` casts rays, sweeps shapes (spheres, or any
  Jolt shape with a rotation), and finds overlaps. `PhysicsScene` does
  the same with entities.
  - Every query can be limited to some layers and can skip bodies, such
    as the caller's own.
  - Triggers are skipped unless asked for.
  - Results give what was hit, where, the surface normal and the
    distance.
- **Blueprint nodes**: Line Trace, Sphere Trace and Overlap Sphere, in a
  new Physics category.
  - Outputs are "hit", "hit entity", "location", "normal" and "distance";
    Overlap Sphere gives an entity array.
  - "Ignore self" is on by default.
  - The game connects them to its physics with
    `BlueprintVM::SetPhysicsQueries`. Without that they find nothing and
    warn (BP207).

**Verified**: 3 new tests.

- A ray straight down lands on the floor's top with an upward normal at
  the right distance, and a ray sideways meets a wall's near face.
- Layer masks, ignored entities and too-short rays miss. Triggers are
  skipped until included.
- A sphere swept down stops at the right height. A fat sphere catches
  what a thin ray passes between. A sweep that starts inside something
  hits at distance 0.
- Overlaps list exactly what's touching, in body order, filtered the same
  way.
- A Blueprint's Line Trace, driven through the VM, hits the floor below
  it and not itself, and its Overlap Sphere counts its two neighbours.
  Without physics connected, the trace misses and warns BP207.
- 295/295 tests pass with physics on GCC 13 (RelWithDebInfo and Debug)
  and under ASan/UBSan, and 273/273 without physics on GCC 13, Clang and
  ASan/UBSan.

**Step 5: character movement** (§13.3).

- **`CharacterMovement`** (`physics/include/aether/physics/character.h`)
  is a capsule character on Jolt's `CharacterVirtual`, with the spec's
  defaults:
  - a 0.35 m × 1.8 m capsule;
  - walking at 4 m/s and running at 7;
  - accelerating at 20 m/s², braking at 25, and 5 in the air with 0.3 air
    control;
  - jumping at 5 m/s, with 0.1 s of coyote time and a 0.1 s jump buffer;
  - 45° slopes and 0.35 m steps.
- **Driving it**: gameplay calls `AddInput(direction)` and `Jump()` and
  sets `run` each step. `CharacterSystem::Step(dt)` moves every character
  and writes its feet to the Transform; run it before the PhysicsScene's
  step.
  - It walks up steps and walkable slopes and slides down steeper ones.
  - It rides moving platforms.
  - It can be teleported by setting its Transform.
- **Events**: Landed (with the impact speed), Jumped and
  MovementModeChanged, delivered to a handler. The new Blueprint events
  OnLanded, OnJumped and OnMovementModeChanged receive them.
- **Triggers see characters**: each character has an inner kinematic body
  that the physics scene maps to its entity. Trigger zones and queries
  name the character.

**Verified**: 6 new tests.

- Walking: it lands and stands at floor height. It walks at 4 m/s,
  covering about 3.6 m in the first second while it speeds up. It runs at
  7 m/s, with over-long input clamped, brakes to a stop in under
  0.3 s, and teleports.
- Slopes: on 30° it stands still for 2 s without creeping, and walks up
  it. On 60° it slides to the bottom and can't walk up.
- Steps: a 0.3 m step is climbed, and a 0.6 m wall blocks it.
- Jumping: it reaches about 1.27 m (v²/2g) and lands at about 5 m/s. A
  second press in the air does nothing. A jump 0.03 s after walking off a
  ledge works, but not after 0.2 s. A press 0.05 s before landing jumps on
  landing, but not one 0.25 s before.
- Triggers and platforms: walking through a trigger enters and exits it
  once, and a ray from above hits the character. A platform moving at
  1 m/s carries a character on it. Removing the component removes the
  character.
- Blueprints receive OnLanded with the impact speed, and
  OnMovementModeChanged with Walking.
- Fix found on the way: gravity applied while standing made characters
  creep down walkable slopes at about 7 cm/s. It now applies only in the
  air.
- 301/301 tests pass with physics on GCC 13 (RelWithDebInfo and Debug)
  and under ASan/UBSan, and 273/273 without physics on GCC 13, Clang and
  ASan/UBSan.

**Step 6: collider gizmos and the physics debug draw**. This completes
Phase 13 on the portable side.

- **Debug draw**: `DrawPhysicsDebug` (`physics/include/aether/physics/debug_draw.h`)
  turns the physics world into colored line segments for a viewport or
  game overlay.
  - It draws colliders, triggers, characters and the last step's contacts,
    each switchable in `PhysicsDebugOptions`, the viewport menu's toggle.
  - Colors: static gray, kinematic blue, dynamic green (darker asleep),
    triggers orange, characters cyan, contacts red.
  - Boxes, spheres and capsules are exact. Convex and mesh colliders use
    the triangles that really collide.
- **Gizmo handles**: `ColliderHandles` gives the drag handles for a
  selected collider, and `DragColliderHandle` resizes it.
  - Boxes have one handle per face, and the opposite face stays put.
  - Spheres have six radius handles.
  - Capsules have four radius handles and two height handles.
  - Handles turn with the entity.
- **Still to come**: the Windows editor draws these and turns drags into
  undoable edits, with its other window hookups.

**Verified**: 3 new tests.

- A box turned 90° has its corners in the right world places. An offset
  sphere's circles lie on its surface, and a capsule spans exactly its
  height and radius. A convex collider with no body shows a cross per
  point.
- The colors match each body, and the toggles hide each kind. A convex
  rock's 12 triangles lie on its cube, and a character is drawn from its
  feet. A landing ball's contact appears with its normal, and a sleeping
  body turns dim.
- Dragging a box face out by 1 m widens it with the far face unmoved;
  shrinking stops at 1 cm. A rotated entity's handles point along its
  turned axes. Sphere and capsule drags resize them, with the capsule's
  bottom fixed and its radius capped at half its height.
- 304/304 tests pass with physics on GCC 13 (RelWithDebInfo and Debug)
  and under ASan/UBSan, and 273/273 without physics on GCC 13, Clang and
  ASan/UBSan.

### Phase 14 (in progress) — The unified renderer

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md),
Phase 14. The editor viewport and the player will share one renderer.
Its CPU side lives in the new `Aether::Renderer` library (`renderer/`),
which builds and tests headless; the GPU passes come later in the phase.

**Step 1: the render scene, views, culling and draw lists** (§14.2).

- **Components**: `DirectionalLight`, `PointLight`, `SpotLight`,
  `SkyLight`, and `PostProcessVolume`.
  - Lights point along their entity's forward.
  - A post-process volume is either global or a box that fades in over a
    blend distance, and sets exposure compensation, bloom, vignette,
    saturation and the tone mapper (ACES, AgX or none).
- **The render scene**: each frame, `ExtractRenderScene` copies the world
  into flat arrays the renderer can use while the game moves on.
  - It takes only active entities, with world transforms through the
    hierarchy and world bounding boxes.
  - Each mesh gets a key, so identical meshes can be drawn together.
  - It collects directional, point, spot and sky light values.
  - It collects post-process volumes.
- **Views**: `MakeView` builds the matrices and the view frustum from a
  camera, or from the scene's active camera.
- **Culling and draw lists**: `Cull` keeps the objects inside the
  frustum. `BuildDrawList` groups them by mesh into instanced batches,
  sorted front to back.
- **Post-processing at a point**: `BlendPostProcess` mixes the volumes in
  priority order. A bounded volume fades in with distance, and its tone
  mapper wins once it counts for at least half.

**Verified**: 4 new tests.

- A child's transform goes through its parent, and inactive entities are
  skipped. Custom and default bounds, and shared and distinct mesh keys,
  come out right. A sun pitched down shines straight down. Light values,
  and spot cones that need clamping, are correct, and so is a turned
  box's world bounds.
- A camera's view puts the origin 10 m ahead. The frustum rejects points
  outside the cone, behind, past the far plane and before the near
  plane, and tests spheres. The view matches the scene's `CameraView`.
  Culling keeps boxes inside, straddling a side, or around the camera.
- Draw lists batch three meshes with instances nearest first, and order
  batches by their nearest instance.
- Post-process volumes blend fully inside, half at 1 m out, and a
  quarter at 1.5 m. The tone mapper switches at half weight.
- 277/277 tests pass on GCC 13, Clang and ASan/UBSan, and 308/308 with
  physics.

**Step 2: light clustering and shadow cascades** (§14.2). These are CPU
references for GPU passes; the GPU versions will be checked against them.

- **Light clusters**: `BuildLightClusters` splits the view into a
  16 × 9 × 24 grid of cells, thin near the camera, and lists which point
  and spot lights reach each one. The forward pass will then light a
  pixel with only its cell's lights, which is what makes hundreds of
  lights affordable.
  - Spot lights are tested with the tightest sphere around their cone.
  - A cell holds at most 256 lights; extras are counted.
  - `ClusterOf` finds a point's cell the way the shader will.
- **Sun shadow cascades**: `ComputeCascades` covers the first 100 m of the
  view with 4 shadow maps, with more detail near the camera.
  - Each map's size depends only on the view's shape, so turning the
    camera never resizes it.
  - Each map moves in whole texels, so walking doesn't make shadow edges
    shimmer.
  - Each map reaches further toward the sun, for shadow casters outside
    the view.

**Verified**: 3 new tests.

- With 200 point and 40 spot lights, in perspective and orthographic
  views, 3,000 random points each find their cell. Every light reaching a
  point is listed there, and lights stay local.
- A far cell doesn't list a small nearby light, and nothing behind the
  camera has a cell. Over-full cells are capped and counted.
- Spot light spheres hold the apex, tip and rim for cones from 10° to 85°.
- Splits match the uniform and logarithmic formulas. Every cascade holds
  its whole slice of the view, plus casters toward the sun.
- Moving the camera 3 mm, 2 cm or 37 cm shifts a fixed point by whole
  texels, and turning it keeps every cascade's size.
- 280/280 tests pass on GCC 13, Clang and ASan/UBSan, and 311/311 with
  physics.

**Step 3: the frame graph's planning** (§14.2).

- **`FrameGraph`** (`renderer/include/aether/renderer/frame_graph.h`)
  plans a frame without depending on D3D12 or Vulkan.
  - Each pass declares its queue (graphics or compute) and what it reads
    and writes, and how: render target, depth, shader read or write,
    copy, and so on.
  - Resources are textures and buffers that exist only during the frame,
    or imported ones such as the swap chain image.
- **`Compile()` works out the frame**:
  - which passes run: a pass whose results nobody uses is dropped;
  - every barrier: state changes, write-to-write barriers between compute
    writes, barriers when memory is reused, and final states for imported
    resources;
  - which passes wait on the other queue, so compute and graphics can
    overlap safely;
  - the frame's temporary memory: resources that are never alive at the
    same time share one heap. In the test frame, the bloom texture reuses
    the shadow map's memory.
  - clear errors for mistakes such as reading something before it's
    written.
- **Next**: carrying the plan out on the GPU comes with step 5.

**Verified**: 3 new tests.

- A forward+ frame: shadows, depth pre-pass, compute light culling,
  forward, an unused debug view, compute bloom, and tone mapping to the
  swap chain.
  - The debug view is dropped, and every expected barrier is there,
    including the swap chain's final return to Present.
  - The compute and graphics passes wait on each other in the right
    places.
  - Lifetimes are right, and the bloom texture reuses the shadow map's
    memory with an aliasing barrier.
- Two compute writes in a row get a UAV barrier, and a side-effect pass
  runs. A graph with no outputs runs nothing. Reading before writing and
  using one resource two ways are reported.
- 200 random graphs: resources alive at once never share memory, the
  output pass always runs, and every running pass's inputs are made by
  earlier running passes.
- 283/283 tests pass on GCC 13, Clang and ASan/UBSan, and 314/314 with
  physics.

**Step 4: post-processing math** (§14.2). These are CPU references for
the post-processing shaders, plus the values the renderer works out each
frame.

- **Auto exposure**: `LuminanceHistogram` and `AverageLuminance` measure
  a frame, ignoring the darkest half and the brightest 5% so the sun
  doesn't set the exposure. `EV100FromLuminance` and `ExposureFromEV100`
  turn that into an exposure, with compensation in EV. `AdaptEV100`
  eases toward it, adjusting to brightness faster than to darkness, like
  an eye.
- **Tone mapping**: `TonemapACES` (Stephen Hill's fit) and `TonemapAgX`
  (Troy Sobotka's AgX), plus sRGB encoding and decoding.
- **Grading**: saturation that keeps brightness, and a vignette.
- **Bloom**: `BloomChain` gives the sizes of the downsampled images, for
  example 960×540 down to 30×17 from 1080p.
- **TAA**: `TaaJitter` gives a Halton(2, 3) sub-pixel offset per frame,
  and `JitterProjection` shifts a projection by it exactly.

**Verified**: 3 new tests.

- For uniform scenes from 0.05 to 40, the exposure maps each to the same
  value. The sun and black pixels are ignored. Adaptation converges,
  brightening faster than darkening, and clamps. Compensation doubles
  per EV.
- Both tone mappers keep black near 0 and huge values at or below 1, and
  are monotonic from 0.001 to 200. Mid grey stays neutral, and negative
  input doesn't produce NaN. A very bright red turns toward white under
  AgX.
- sRGB matches known values and round-trips.
- Saturation keeps luminance, and the vignette is untouched at the
  center. The bloom chain from 1080p matches, as do known Halton values
  and a jitter centered on average. A jittered projection moves points by
  exactly 0.25 and −0.4 pixels at every depth, for perspective and
  orthographic, without touching depth.
- 286/286 tests pass on GCC 13, Clang and ASan/UBSan, and 317/317 with
  physics.

### Phase 15 (in progress) — Materials

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md),
Phase 15. Materials are node graphs with typed pins that will compile to
HLSL, like Unreal's material editor. They live in `Aether::Renderer`
(`aether/renderer/material.h`, namespace `aether::mat`).

**Step 1: the material model** (§15.2).

- **Material**: shading model (Default Lit or Unlit), blend mode (Opaque,
  Masked, Translucent, Additive), two-sided, the mask clip value, named
  parameters (scalar, vector or texture, with defaults and groups),
  nodes and links.
- **Node library** (`ResolveNode`, `ListNodeTypes` for the palette):
  - the Material Output node (BaseColor, Metallic, Roughness, Normal,
    Emissive, AO, Opacity, OpacityMask, WorldPositionOffset);
  - constants, and parameter nodes (`Param.Scalar:Roughness`);
  - Texture Sample (a texture input is required; UVs default to UV0),
    Unpack Normal;
  - TexCoord, Time, World Position, Vertex Normal, Camera Vector;
  - generic math (Add to Normalize, Lerp, Clamp, Dot, Length), Append,
    Component Mask, Fresnel and Panner.
- **Types**: generic math pins take the widest connected input, and a
  float broadcasts to any size. A wider value into a narrower pin is
  truncated, with a warning.
- **Validation** (`Analyze`), with codes:
  - MT001: no Material Output, or more than one.
  - MT002: an unknown node or a bad config.
  - MT003: incompatible types.
  - MT004: a missing pin or node.
  - MT005: a cycle.
  - MT006: an unknown parameter.
  - MT007: a required input left unconnected.
  - MT008: an input with two links.
  - MT009 (warning): a pin the material's settings ignore.
  - MT010 (warning): truncation.
- **Files**: `.amat` JSON with a version, via `SaveMaterial` and
  `LoadMaterial`.

**Verified**: 5 new tests.

- Type names, parsing and every kind of fit.
- Nodes resolve, and bad configs and parameters are refused. The palette
  lists the library and the parameters.
- Inference: float3 × float3, float3 × float, and float2 appended to a
  float giving float3, which is then masked and dotted.
- Each diagnostic code is produced by a graph that should produce it.
- JSON and file round trips are exact, and bad files are refused.
- 291/291 tests pass on GCC 13, Clang and ASan/UBSan, and 322/322 with
  physics.

**Step 2: HLSL generation** (§15.3, `aether/renderer/material_codegen.h`).

- **`GenerateHlsl`** turns a valid material into a shader include:
  - a `MaterialParams` constant buffer;
  - the textures it samples;
  - `EvaluateMaterial` for the pixel stage;
  - `EvaluateWorldPositionOffset` for the vertex stage, which samples with
    `SampleLevel`.
- **Only what's used**: nodes that don't reach an output are dropped, and
  so are the ones behind pins the settings ignore (Opacity on an Opaque
  material, everything except Emissive on Unlit). Identical expressions
  are computed once.
- **Parameter layout**: widest first, each into the first gap that
  doesn't cross a 16-byte register, with explicit `packoffset`s.
  Parameter names become safe, unique identifiers (`P_Base_Tint`).
- **Permutations**: defines for shading, blend, two-sided, mask clip,
  world position offset, UV count and the inputs used. There is also a
  64-bit key that ignores node positions and parameter values.
- Stats for the editor: live node and instruction counts.

**Verified**: 5 new tests.

- The buffer layout matches HLSL's rules, and identifiers are made safe
  and unique.
- The surface code matches expected lines, including broadcasts,
  truncation and per-node defaults. Invalid materials don't generate.
- Dead nodes and ignored pins emit nothing. Two identical multiplies and a
  sample read three times are each computed once.
- A graph using every node type samples with `SampleLevel` in the vertex
  stage, gives only sampled textures a slot, and reports its features.
  Keys change with code and settings only.
- When `glslangValidator` is installed, the output of five materials
  compiles as HLSL (lit, unlit, translucent, empty, and the graph with
  every node type).
- 296/296 tests pass on GCC 13, Clang and ASan/UBSan, and 327/327 with
  physics.

**Step 3: material instances** (§15.4, `aether/renderer/material_instance.h`).

- **`.amati` instances** override a parent's parameters. The parent is a
  material or another instance. `LoadInstanceChain` follows parents up
  to the material and reports loops, missing parents and chains that are
  too deep (MI003–MI005).
- **Resolution** applies the overrides parent first, and records where
  each value came from. An override for a renamed or retyped parameter
  is skipped with a warning (MI001 and MI002), so it doesn't break.
- **No recompiling**: an instance only changes data. `PackParameters`
  writes the MaterialParams bytes at the generated layout's offsets, and
  `TextureBindings` gives each texture slot's GUID.
- **`ParameterBlock`** is the runtime version, like Unreal's dynamic
  material instance. You set values, it repacks on the next read, and
  its revision changes only when a value actually does.
- **`MaterialParameters` component**: the material or instance an entity
  draws with, plus its own overrides. Its Blueprint-callable methods
  become Blueprint nodes: Set Scalar, Set Vector and Set Texture
  Parameter, Get Scalar and Get Vector Parameter, and Clear Parameter.
- **Rendering**: extraction carries each object's material key and
  overrides, and draw batches group by mesh and material.

**Verified**: 4 new tests.

- Overrides, JSON and file round trips, and five kinds of broken file.
- A two-level chain resolves in order, with sources and both warnings.
  The chain loads from an in-memory asset store, and a loop, a missing
  parent, a non-material parent and a 20-deep chain are refused.
- Packed bytes land at the layout's offsets. The block repacks, skips
  unchanged values, refuses the wrong kinds, and resets. The shader key
  is unchanged.
- A Blueprint sets Roughness and Tint on an entity and reads Roughness
  back. The overrides reach a block, and reapplying them costs nothing.
  Extraction and batching split two materials over one mesh.
- 300/300 tests pass on GCC 13, Clang and ASan/UBSan, and 331/331 with
  physics.

**Step 4: functions, custom code, noise and triplanar** (§15.5).

- **Material functions** (`.amf`) are reusable subgraphs with Function
  Input and Function Output nodes. Materials call them with
  `Function.Call:<name>` nodes, and functions can call other functions.
  - Codegen inlines each call. The inputs and outputs become typed
    reroutes, so a function returns exactly the type it declares.
  - Errors: an interface problem (MT011), an unknown or recursive call
    (MT012), or a broken function (MT013) is reported on the call.
- **Custom HLSL node**: typed inputs, an output type and a function body.
  Identical nodes share one helper function.
- **Noise**: 3D gradient noise, with up to 8 octaves of fBm. It uses
  world position by default.
- **Triplanar Sample**: a texture projected along X, Y and Z, blended by
  the normal. It uses `SampleLevel` in the vertex stage.
- **Reroute** node, typed.

**Verified**: 3 new tests.

- Function interfaces, palettes and every error code, including
  recursion through two functions. Functions round-trip as
  `MaterialFunction` files.
- A function that calls another function twice inlines with no calls
  left. Each call keeps its own defaults, and a declared float3 output
  keeps its type. The original graph is untouched, and the output
  compiles.
- Custom, Noise (1 and 4 octaves) and Triplanar (pixel and vertex)
  generate the expected helpers and calls, and compile. Bad Custom and
  Noise configs, and an unsampled Triplanar, are refused.
- 303/303 tests pass on GCC 13, Clang and ASan/UBSan, and 334/334 with
  physics.

**Step 5: the material editor** (portable, in `aether_editor_ui`).

- **`MaterialDocument`**: the open material or function, its file, and
  whole-material undo/redo (drags merge into one step). The generated
  shader is cached per revision.
  - Parameters: add, rename (their nodes follow), retype (their nodes
    change kind, and links that no longer fit are broken) and remove
    (with their nodes).
  - Node copy, paste and duplicate, keeping internal links. The Material
    Output is never copied.
- **Graph adapter** (`material_graph.h`): builds the Phase 12 widget's
  view.
  - Pins are colored by type; generic pins take their inferred type.
  - Headers are colored by category, and errors show on their nodes.
  - Links are type-checked. A texture into math, a narrower vector into
    a wider one, and a link that would loop are refused with a reason.
    A new link into an input replaces the old one.
  - The Material Output can't be deleted.
  - The palette is fuzzy-searched. When opened from a dragged wire, it
    only lists nodes that could link to that pin.
- **`MaterialEditor` panels**:
  - toolbar: Save, Undo/Redo and the error count;
  - Parameters: grouped, and draggable into the graph;
  - the graph;
  - Details: the material's settings, a parameter (name, type, default,
    group), or a node (constants, mask channels, UV set, noise octaves,
    reroute and function interface types, Custom node inputs and code,
    and defaults for unconnected inputs);
  - a bottom panel: Stats (live nodes, instructions, textures, buffer
    size, permutation key, defines), Diagnostics (click to frame the
    node) and the generated HLSL.
- The live preview (a lit sphere) and the Windows editor hookup need the
  GPU and come with step 6.

**Verified**: 5 new tests (headless ImGui).

- Document: undo, redo and merged drags, and the parameter edits with
  their effect on nodes and links. The shader cache, the function
  library surviving undo, and files.
- Clipboard: internal links kept, the output skipped, bad pastes leaving
  no undo step, and duplicates offset.
- Graph: pin and wire colors, every refused link and its reason, and a
  broadcast link replacing the old one. Output-safe deletes, wire-drop
  auto-linking, and errors on nodes.
- Palette: ranking, and filtering for a texture output and for a float3
  input.
- Panels: the palette places a linked Triplanar; every Details variant
  and bottom tab draws. Parameter nodes, renames, forgetting undone
  selections, a failed save, and a function document.
- 308/308 tests pass on GCC 13, Clang and ASan/UBSan, and 339/339 with
  physics.

### Phase 16 (in progress) — Animation

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md),
Phase 16. The runtime is the new `Aether::Animation` library
(`animation/`), which builds and tests headless.

**Step 1: skeletons, poses, clips and blending** (§16.1).

- **`BuildSkeleton`** turns an imported model's skin into bones.
  - Parents come first, and each bone's parent is its nearest joint
    ancestor.
  - Non-joint nodes in between, or above the root, are folded into the
    rest poses.
  - A matrix node is decomposed into translation, rotation and scale.
  - Duplicate, missing and out-of-range joints are refused.
- **Poses**: `LocalToModel` and `SkinMatrices` (model × inverse bind).
- **Clips**: `BuildClip` maps an imported animation's channels onto the
  bones. Sampling handles linear and step keys, clamping or looping, and
  falls back to the rest pose.
- **Keyframe reduction** removes keys within translation, rotation and
  scale tolerances. It checks between keys too, because rotations aren't
  linear. Constant tracks shrink to one key.
- **Compression**: 16-bit quantized times and values, with smallest-three
  rotations. A 6-bone test clip is more than 2x smaller than plain floats,
  and decoding checks bounds.
- **Blending**: pose blends (optionally through a bone mask with a soft
  ramp), and additive animation made from and applied to a reference.
- **Rotation helpers**: nlerp, slerp, conjugate, rotating a vector, and
  an angle measure that stays precise for small angles.

**Verified**: 6 new tests.

- The rotation helpers.
- A skeleton from an out-of-order skin with an intermediate node, with
  model positions and identity skin matrices at rest. A matrix joint
  round-trips, and five kinds of bad skin are refused.
- Sampling: linear, step, clamped and looping, with rest fallback. A
  non-bone channel is skipped.
- Reduction: a line shrinks to 2 keys, a constant track to 1, and held
  keys merge. Over 1000 samples of a sine wave and a wobble, the error
  stays within tolerance.
- Compression: round-trip errors of about 3e-5 m and under 2e-4 rad.
  Step flags survive, every truncated prefix is refused, and bad
  indices are caught.
- Blending by weight and by upper-body mask, a soft mask ramp, and
  additive round trips.
- 314/314 tests pass on GCC 13, Clang and ASan/UBSan, and 345/345 with
  physics.

**Step 2: blend spaces and root motion** (§16.3).

- **Blend spaces** (`.ablend`):
  - 1D, and 2D with Delaunay triangulation and barycentric weights. The
    nearest hull point is used outside the samples, and collinear
    samples blend along their line.
  - Diagnostics BS001–BS006.
- **`BlendSpacePlayer`** keeps every clip at the same phase. A cycle
  takes the weighted average of the clip durations, so walk and run
  stay in step while the speed changes.
- **`BlendWeighted`** blends any number of poses. Rotations are aligned
  first, so opposite-signed quaternions don't cancel out.
- **Root motion**: `ExtractRootMotion` gives the root's movement over
  the ground and its heading, in its own frame, across loop wraps.
  `StripRootMotion` plays in place. Blend spaces hand out the blended
  motion.

**Verified**: 5 new tests.

- 1D weights: in any order, clamped and at the samples.
- 2D weights: a cross reproduces 200 random points. Outside the hull
  uses the nearest edge. A 30-sample cloud passes the Delaunay
  empty-circle check, and points inside it are reproduced.
- Every diagnostic, collinear fallback blending, and `.ablend` round
  trips and bad files.
- Weighted poses, including a negated quaternion. The walk/run sync
  matches separately sampled clips, and phase is kept across a
  parameter change.
- Root motion: halves, composed steps, loop wraps, settings and
  stripping. A blend-space player travels the average distance while
  playing in place.
- 319/319 tests pass on GCC 13, Clang and ASan/UBSan, and 350/350 with
  physics.

**Step 3: animation graphs and state machines** (§16.4, `aether/animation/anim_graph.h`).

- **Graph** (`.aanim`): typed variables (bool, int, float, trigger) and a
  DAG of pose nodes:
  - Clip;
  - Blend Space;
  - Blend by alpha, by bool and by int (with crossfades);
  - Layered per bone;
  - Additive;
  - State Machine.
- **State machines**:
  - Transitions have conditions that must all hold, a priority, a blend
    time and an optional "when the animation finishes" rule.
  - Any-state transitions, conduits (pass-through decision states) and
    sub-machines (a state playing another machine, restarting at its
    entry).
  - The state being left keeps playing during a crossfade. Interrupting
    a blend blends from a snapshot, with no pop.
  - Each Update's state changes are reported.
- **`AnimGraphInstance`**: one character's variable values and playback
  state. A node shared by two parents advances once per frame.
- **Diagnostics** AG001–AG011, from loops and bad inputs to type errors
  in conditions and unreachable states.

**Verified**: 5 new tests.

- Locomotion: a quarter-per-frame blend into Walk. An any-state Jump on
  a trigger outranks Walk → Run and uses up the trigger. Jump → Idle
  only near Jump's end, and unused triggers expire.
- A jump mid-blend continues from the blended pose.
- Conduit pass-through, and staying put when no exit holds. A sub-machine
  inside an outer machine restarts at its entry after death and revive.
- Every blend node's result, and the shared node advancing once.
- Every diagnostic, and `.aanim` round trips and bad files.
- 324/324 tests pass on GCC 13, Clang and ASan/UBSan, and 355/355 with
  physics.

**Step 4: Animator, montages, notifies and root motion** (§16.5).

- **Notifies**: instant and window notifies on clips, reported as the
  playhead crosses them (across loop wraps too), each with its path's
  weight. Blend spaces step once. Compressed clips carry notifies (format
  v2, and v1 still loads).
- **Montages** (`.amontage`):
  - a clip on a slot with blend in and out;
  - sections that chain, loop or end, plus Jump To Section and Stop;
  - replacing a montage takes over its weight without a pop;
  - a `Slot` node in the graph shows them.
- **Events**: notifies, state changes and montage progress, as
  `AnimEvent`s and as eight new Blueprint events (`Event.OnAnimNotify`,
  `Event.OnMontageEnded`, ...).
- **Root motion**: graphs and montages play in place, and their weighted
  motion is handed to the character. `CharacterMovement::AddRootMotion`
  moves the capsule by it for one step and turns the entity.
- **`Animator` component**: graph, skeleton, root motion, speed, pause.
  Its Blueprint-callable Set Parameter, Set Trigger, Play/Stop Montage
  and Jump To Section queue commands.
- **`AnimationSystem`**: runs every Animator, in parallel jobs when given
  a `JobSystem`. It keeps poses, skin matrices, events and world-space
  root motion, and reports missing assets once.

**Verified**: 6 new tests.

- Notify crossing rules, and compression v2 and v1.
- Graph notifies: two steps per walk cycle, one per cycle from a blend
  space, and crossfade weights.
- Montages:
  - blend-in weights;
  - a looping section, hits and window notifies;
  - jumping to the end and finishing on its own;
  - stopping (interrupted), and replacement taking over the weight.
  - Files, MN001–MN004, and a slot without a name.
- Root motion: in place at 2 m/s across loop wraps, a half-blended
  montage adding its share, and none when it's off.
- Animator:
  - commands and events per entity, missing assets, and dropped
    entities;
  - 12 characters in parallel jobs match serial exactly;
  - a Blueprint plays a montage and hears its notify and its end.
- With physics: a character facing +X walks 3 m along X in 2 s from the
  clip's +Z root motion, stays grounded, and stops when the motion stops.
- 329/329 tests pass on GCC 13, Clang and ASan/UBSan, and 361/361 with
  physics.

**Step 5: IK and retargeting** (§16.6, `ik.h`, `retarget.h`).

- **Two-bone IK**: analytic, with a pole vector. It handles limbs that
  start straight, straightens toward targets out of reach, and blends
  by weight.
- **Look-at**: an aim axis toward a target, within an angle limit.
- **FABRIK** for chains such as tails and spines: it keeps the root and
  the bone lengths.
- **Foot placement** (`FootPlacer`): a ground trace per foot, the pelvis
  dropped for the lowest foot, leg IK keeping each knee's bend plane,
  feet tilted to the ground, and smoothing over time.
- **Retargeting**:
  - Rotations carry over in model space relative to each rig's rest
    pose, so rigs with different bone axes still match.
  - The root's movement scales by the rigs' hip heights.
  - Bones pair automatically by name (`mixamorig:Hips` → `hips`), and
    whole clips can be retargeted with their notifies.

**Verified**: 4 new tests.

- Two-bone IK: the hand reaches the target with bone lengths kept, and
  the elbow follows the pole to either side. It also works from a bent
  start, out of reach, too close, at half weight, and refuses a non-chain.
- Look-at: aiming, and the angle limit. FABRIK on a five-bone tail: it
  reaches, lengths hold, and it goes straight when out of reach.
- Foot placement: flat ground, a 0.3 m step, a 0.2 m dip (the pelvis
  drops 0.2 m), an out-of-range cliff, a slope tilting the foot, and
  smoothing.
- Retargeting between a 1 m and a 2 m rig with a twisted arm bone:
  directions match, target lengths are kept, the root moves twice as
  far, and rest maps to rest. Clip resampling and explicit mappings.
- 333/333 tests pass on GCC 13, Clang and ASan/UBSan, and 365/365 with
  physics.

**Step 6: the animation editors** (portable, in `aether_editor_ui`, `editor/src/anim/`).

- **`AnimGraphDocument`**:
  - Undo and redo through JSON snapshots.
  - Renaming a variable, machine or state updates everything that names
    it. Retyping a variable drops conditions that no longer fit, and
    removing one clears its uses.
  - Removing a state fixes transition indices and the entry. New states
    get a Clip node for their pose.
  - Pose links refuse loops, and deleting a node unsets its users.
  - Graphs save as `.aanim`, now with node and state positions.
- **Graph editor** (`AnimGraphEditor`):
  - The pose graph, with kind-specific input pins and an Output Pose
    node.
  - A tab per state machine, with Entry, Any State and conduit nodes and
    transitions drawn as wires.
  - During PIE the live state is highlighted and the blending
    transition glows.
  - Panels: Variables (with live values during PIE), Details for nodes,
    states, transitions (conditions, blend, priority, when-finished) and
    variables, and clickable Diagnostics.
  - An add-node menu.
- **Blend space editor**:
  - A grid with the triangulation, and samples you drag with snapping.
    Right-click adds a sample and Delete removes one.
  - A preview dot shows live weights.
  - The axes, sample details, validation, and undo.
- **Clip viewer**: play, pause, loop, speed and scrubbing, and the bone
  hierarchy. A notify track where you add, drag, rename, delete and
  undo notifies. Translation curves for the selected bone.
- The 3D preview comes with the renderer's skinning (step 7).

**Verified**: 5 new tests (headless ImGui).

- The document's cascading renames and removals, retypes, loop refusal,
  node deletion, state removal index fixes, machine removal, and files.
- The views' pins and pseudo nodes, and pose edits (named pins, the
  output, breaks, moves). Machine edits: entry, any-state, a
  self-transition refused, disconnects, and state deletion.
- Blend space: snapped and clamped drags as one undo step, collinear
  detection, and canvas mapping and hit testing.
- Clip viewer: paused, playing with notifies, looping, stopping at the
  end, notify editing with undo, and curves.
- Every panel and Details view draws. Palette placement of every node
  kind and a conduit, live highlighting, and a failed save.
- 338/338 tests pass on GCC 13, Clang and ASan/UBSan, and 370/370 with
  physics.

### Phase 17 — Audio

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md),
Phase 17. The engine side is the new `Aether::Audio` library (`audio/`).
It is a software mixer that renders into buffers, so it is tested
headless; the device output backend (miniaudio) pulls from it and comes
in step 5.

**Step 1: sounds, voices, buses and effects** (§17.1).

- **Sounds**:
  - WAV decoding: 8-, 16-, 24- and 32-bit PCM and 32-bit float, mono or
    stereo, including extensible headers.
  - Unknown chunks are skipped. A data chunk that claims more than the
    file holds plays what's there.
  - WAV encoding, and a tone generator.
- **Voices**:
  - Cubic resampling from any rate, and pitch.
  - Volume in dB, smoothed so changes don't click.
  - Equal-power pan for mono sounds and balance for stereo.
  - Linear fades in and out, looping, and starting part way in. A voice
    frees itself when it ends or finishes fading out.
- **Buses**:
  - Master with Music, SFX, UI and Voice, plus any buses you nest under
    them.
  - Each has a volume, a mute that ramps without a click, and effects
    in order.
  - Peak and RMS meters per channel.
- **Effects**:
  - RBJ biquad filters (low- and high-pass, band-pass, notch, shelves,
    peak), with their analytic response for the editor.
  - A feed-forward compressor that reports its gain reduction.
  - A Freeverb reverb.

**Verified**: 4 new tests.

- WAV:
  - 16-bit and float round trips, and hand-made 8- and 24-bit files;
  - extensible headers, junk chunks and a short data chunk;
  - six kinds of bad file refused.
- Voices:
  - a 44.1 kHz tone at 48 kHz stays 440 Hz and ends on time; pitch 2 is
    880 Hz at half the length;
  - -6 dB, hard left, equal-power centre, a smoothed volume change;
  - looping, fading out and in, starting at 0.5 s, and stereo balance.
- Buses: volumes multiply down the tree, and the meters read each bus.
  Muting ramps then silences, and nothing playing is silence.
- Effects:
  - filter responses at their key frequencies, and a tone through a bus
    low-pass matching the analytic gain;
  - bypass;
  - the compressor reducing 12 dB over the threshold by 9 dB at 4:1, and
    leaving quiet signals alone;
  - a reverb tail that decays, and dry-only passthrough.
- 342/342 tests pass on GCC 13, Clang and ASan/UBSan, and 374/374 with
  physics.

**Step 2: 3D audio and voice limiting** (§17.2).

- **Attenuation**:
  - Inverse (with rolloff), linear, logarithmic, or a custom curve,
    between a min and a max distance.
  - Optional air absorption: a low-pass that closes from open at the min
    distance to a set cutoff at the max, swept in octaves.
- **3D voices**:
  - Panned from the listener's position and facing.
  - A spatial blend from plain 2D to fully 3D.
  - Doppler from source and listener velocities (the OpenAL formula),
    clamped, and multiplied with the voice's pitch.
  - Gain changes from moving glide like volume changes; a voice starts
    at its 3D level with no glide.
- **Occlusion**:
  - A hook the game fills (typically a raycast) that says how blocked a
    source is, called by `UpdateOcclusion` on the game thread.
  - It ducks (-12 dB) and low-passes (1.2 kHz) at full occlusion,
    smoothed over 0.1 s. Hosts can also set it per voice.
- **Voice limiting**:
  - At most N voices mix, chosen by priority, then by how loud they'd
    reach Master (including distance, occlusion and buses).
  - Voices under -80 dB go virtual even when channels are free, including
    those on a muted bus.
  - A virtual voice keeps time (Continue), comes back from the start
    (Restart), or ends (Stop). A voice fades over one block going virtual
    or coming back, so there's no click.
- `GetVoiceInfo` reports each voice's distance, gains, pan, pitch,
  filter and whether it's virtual, for the editor and debugging.

**Verified**: 4 new tests.

- The maths:
  - every attenuation model at known distances, and all four falling
    monotonically;
  - air-absorption cutoffs;
  - panning, including a turned listener;
  - doppler toward, away, sideways, with a moving listener, turned off,
    and faster than sound.
- 3D voices:
  - a quarter of the level, all in the right ear, at 4 m;
  - centred and at full level with no jump when the listener walks to the
    source;
  - half spatial blend, and blend 0 behaving as 2D;
  - a tone 11% sharp approaching and flat receding;
  - distance filtering of a high tone but not a low one.
- Occlusion:
  - a wall hook ducking and filtering only the voices that ask for it,
    smoothly;
  - clearing when the listener steps through;
  - set directly on a 2D voice.
- Voice limiting:
  - priority and loudness deciding who plays;
  - Continue coming back on time, Restart from the top, Stop ending, and
    a one-shot freed while virtual;
  - ramps with no click in and out;
  - out-of-range and muted voices going virtual and coming back.
- 346/346 tests pass on GCC 13, Clang and ASan/UBSan, and 378/378 with
  physics.

**Step 3: decoding, streaming and sound cues** (§17.3).

- **Formats**:
  - Ogg Vorbis (stb_vorbis) and FLAC (dr_flac) alongside WAV, told apart
    by their headers.
  - They decode whole into a sound, or stream from memory or from disk.
- **Streaming voices**:
  - Decode ahead in chunks as they play, loop by seeking, and seek again
    when they jump (start time, coming back from virtual, restart).
  - They mix exactly the same samples as the decoded sound.
- **Delayed starts**, to the sample, and a frame clock on the mixer.
- **Sound cues** (`.acue`):
  - Wave, Random (weights, no repeats), Sequence, Modulator (volume and
    pitch ranges), Concatenator, Loop (a count or forever), Mix and Delay
    nodes, with the output's volume, pitch, bus, priority and 3D
    settings.
  - Evaluating one gives the sounds to play and when. An endless loop
    over a single sound is one looping voice; over anything else, it is
    evaluated again each time round, so variations change.
  - Validation (CU001–CU011) and JSON files.
- **Cue player**:
  - Plays cues from a sound bank (decoded or streamed sounds).
  - Places every sound from the cue's start frame, so concatenations and
    loops are seamless and don't drift.
  - Schedules endless loops a quarter-second ahead.
  - Stops, moves and changes the volume of each playing cue.

**Verified**: 4 new tests, with an Ogg and a FLAC file in
`assets/audio/`.

- Decoding:
  - the Ogg's pitch, length and level;
  - the FLAC matching the source samples exactly;
  - bad and cut-short files refused;
  - streams read in chunks and seek to the same samples, from memory
    and from disk.
- Streamed voices mix the same samples as decoded ones:
  - once through, looping, and Vorbis with a pitch and start time;
  - losing and regaining a channel after wrapping while virtual.
- Delayed starts land on the exact frame across blocks.
- Cues:
  - each node's timing, pitch and volume;
  - counted, endless and intro-then-loop loops;
  - weighted and no-repeat random choices, sequences and modulation,
    repeatable from a seed;
  - every validation code;
  - `.acue` round trips and malformed files.
- Cue player:
  - concatenations and an endless loop that match the expected samples
    exactly for a second;
  - streamed cues;
  - volumes, buses and fades;
  - 3D positions;
  - sequences across plays.
- 350/350 tests pass on GCC 13, Clang and ASan/UBSan, and 382/382 with
  physics.

**Step 4: components, the audio system and Blueprint nodes** (§17.4).

- **Components**:
  - **AudioSource** plays a cue from an entity and follows it, with
    doppler from its motion. Its Blueprint nodes are Play, Stop, Fade
    In, Fade Out, Set Volume, Set Cue and Is Playing.
  - **AudioListener** hears from its entity's position and facing.
  - **ReverbZone** gives the listener a reverb that fades in towards the
    zone. Overlapping zones go by priority.
- **AudioSystem**:
  - Runs the components on the mixer each frame.
  - Follows the scene hierarchy.
  - Reports cues that finish (as `Event OnAudioFinished`), and missing or
    broken cues once each.
  - Stops the sounds of entities that go away.
- **Audio Blueprint library**: Play Sound 2D, Play Sound at Location,
  Spawn Sound Attached (follows an entity and stops with it), Set Bus
  Volume (with a fade) and Stop All Sounds.
- **Blueprints**:
  - Native functions can now take entities (Entity pins).
  - Static function libraries get their own palette category.

**Verified**: 3 new tests.

- Sources and the listener:
  - panning from a turned listener;
  - distance and doppler from motion, and listener velocity;
  - every command;
  - the finished event, reported once;
  - switching cues, auto-play off, and destroyed entities;
  - missing and broken cues;
  - parented sources.
- The library:
  - 2D against located sounds;
  - attached sounds following, turning and stopping;
  - bus volume at once and faded;
  - the static functions with and without an active system.
- Reverb zones:
  - no effect without zones;
  - dry outside and a tail inside;
  - the blend at the edge, and priority over strength.
- A Blueprint that plays a 2D sound, attaches a hum, fades a bus and fades
  in its own source, then prints the cue from `Event OnAudioFinished`.
- 353/353 tests pass on GCC 13, Clang and ASan/UBSan, and 385/385 with
  physics.

**Step 5: the output device and the audio thread** (§17.5).

- **Backends**:
  - miniaudio for the platform's device (WASAPI, Core Audio, ALSA,
    PulseAudio, ...).
  - A null backend, real time or pumped by hand, which can capture its
    output.
  - A default that falls back to null when there's no device.
- **The mixer on its own thread**:
  - Game-thread changes go to the audio thread through a lock-free queue
    and are applied in order before each block.
  - The audio thread publishes a snapshot (voices, meters, the clock)
    through a triple buffer.
  - Nothing on the game thread waits for the audio thread, and the audio
    thread never waits for anything.
  - Voice ids come back at once, and a sound just played already counts
    as playing.
  - Scheduled sounds subtract the time they spent queued, so cue loops
    stay sample-accurate while the game schedules them live.
- **Existing code runs threaded unchanged**: CuePlayer and AudioSystem
  work the same either way; the AudioSystem's reverb changes go through
  `Post`.

**Verified**: 4 new tests, clean under ThreadSanitizer too.

- The queue, across two threads (200,000 items in order).
- A threaded mixer, pumped from another thread, producing exactly the same
  samples, meters, counts and voice info as a mixer on one thread, over
  eight rounds of changes. Queued plays and stops are seen at once.
- A real-time audio thread under 3,000 random plays, moves, stops and
  occlusion updates, plus 20,000 commands at once (more than the queue
  holds).
- A cue loop scheduled live from the game thread staying sample-exact
  for a quarter second.
- miniaudio's null device pulling audio on its own thread and playing a
  sound through.
- 357/357 tests pass on GCC 13, Clang and ASan/UBSan, and 389/389 with
  physics.

**Step 6: the audio editors** (§17.6).

- **Waveform preview**:
  - Peaks and RMS per column for each channel, zooming down to single
    samples.
  - Peak, RMS, DC offset and clipping statistics.
  - A playhead, a selected region, and preview playback that loops the
    selection.
- **Sound cue editor**:
  - The cue graph with an Output node, "+" pins to add inputs, error
    outlines, and a searchable palette of node types and sounds.
  - Details for every node type and for the output's settings, including
    the attenuation curve plotted.
  - Undo, diagnostics, save, and preview playback (Space).
- **Mixer panel**:
  - Channel strips with live L/R meters (peak hold, RMS), faders and mute.
  - Effect settings and bypass, read and written on the audio thread when
    the mixer is threaded.
  - A live voice list (real or virtual, distance, gain, pan, pitch).
- **Sync groups** for cues on a threaded mixer: a cue's sounds are placed
  from a start frame the audio thread fixes. A lagging game clock can no
  longer shift the first sound against the rest (ThreadSanitizer's slower
  runs showed it).

**Verified**: 5 new tests, headless, plus a sync-group check on the
threaded mixer. Clean under ThreadSanitizer.

- Waveform:
  - peaks of a tone, DC, one channel and single samples;
  - statistics and clipping;
  - zoom about a point, scroll limits, playhead and selection clamps;
  - preview following the voice and looping a selection.
- Cue document:
  - adding, linking (replace, append, and refused links with reasons);
  - disconnecting with weights, deleting (and unhooking the output);
  - undo/redo, no-op edits, unknown sounds, and save/load.
- Cue graph:
  - pins, titles and links;
  - errors on nodes;
  - every widget edit applied (connect either way, the output, refused
    links, disconnects, moves, deletes);
  - the attenuation plot.
- Cue editor:
  - every Details panel, the output's with a custom curve, and the
    palette;
  - selection dropped on undo;
  - preview blocked by errors, then played and stopped.
- Mixer panel:
  - meter ballistics;
  - effect parameters by kind;
  - meters, faders, mute, effect settings and bypass;
  - the voice list and every tab, on a single-threaded and a threaded
    mixer.
- 362/362 tests pass on GCC 13, Clang and ASan/UBSan, and 394/394 with
  physics.

### Phase 18 (in progress) — Runtime game UI

Build spec: [`docs/design/PHASE_SPECS.md`](docs/design/PHASE_SPECS.md),
Phase 18. The engine side is the new `Aether::UI` library (`ui/`): a
retained widget tree that paints into a draw list of clipped quads for the
renderer, so its layout, input and styles are tested headless.

**Step 1: widgets, layout and drawing** (§18.1).

- **Widget tree**:
  - Names, visibility (visible, hidden, collapsed, and two kinds that let
    clicks through), opacity down the tree, and clipping.
  - Two-pass layout: children measure first, then parents arrange.
- **Panels**:
  - Canvas with Unity/UMG-style anchors: point or stretched per axis,
    with a pivot, margins, auto-size and z order.
  - Horizontal and Vertical boxes with fill weights and alignment.
  - Grid with auto and weighted tracks and spans.
  - Overlay, SizeBox, Border, Spacer.
  - ScrollBox with clipping, a scroll bar and scroll-into-view.
- **Text and Image**:
  - Line breaks and word wrap, alignment, and UTF-8.
  - Brushes: colour, image, and 9-slice box or frame.
- **Draw list**: textured, tinted quads with nested clip rectangles,
  culling the empty, transparent and clipped-away.
- **Viewport**:
  - Layers in z order laid out over the safe area.
  - Resolution scaling against a reference resolution (shortest or
    longest side, width, height, or a DPI curve).
  - Painting in pixels, and hit testing top layer first.

**Verified**: 3 new tests.

- Text: widths, lines, wrapping (including a word too long for a line),
  empty text and UTF-8.
- Draw list: nested clips, culling, 9-slice rectangles and UVs, frames,
  corners shrinking, glyph placement and alignment, and scaling to pixels.
- Canvas: centred, stretched, bottom-bar and auto-sized children, a
  canvas in a smaller rect, and z-ordered hits.
- Boxes (weights, padding, alignment, collapsed against hidden), Grid
  (auto, weighted and spanning), Overlay, SizeBox limits, Border, and
  ScrollBox (limits, scroll-into-view, clipped hits and paint, scroll
  bar).
- Scale rules and clamping; a 4K screen with a 1080p layout and a safe
  area; layer order and removal; every visibility; opacity; tree edits.
- 365/365 tests pass on GCC 13, Clang and ASan/UBSan, and 397/397 with
  physics.

**Step 2: controls and input** (§18.2).

- **Controls**:
  - Button, Toggle and Slider (drag, snap, arrow steps).
  - ProgressBar (from any side).
  - TextInput: cursor, selection, UTF-8 editing, length limits,
    passwords, and scrolling long text.
  - Dropdown, with a popup that keeps focus.
  - ListView, which only makes widgets for the rows in view.
  - Tooltips.
- **Input router**:
  - Mouse and multi-touch with hover and capture; the wheel bubbling to
    scroll boxes.
  - Focus with spatial and tab navigation, explicit targets, and
    scroll-into-view.
  - Accept and cancel, text editing, popups and tooltips.
  - Each call says whether the UI used the input, so gameplay can skip
    it.
- **Device bridge**:
  - Arrows, d-pad and stick navigation with key repeat; Tab; Enter, Space
    and A accept; Escape and B cancel.
  - Text-box-aware keys.
  - A modal context that blocks gameplay input while a menu is up.

**Verified**: 3 new tests.

- Pointer:
  - hover, press, capture, and release inside or outside;
  - right-click, and accept;
  - disabled controls and parents;
  - toggles, and sliders dragged, clamped, snapped and stepped;
  - two fingers on two sliders;
  - progress fills;
  - the wheel passing over a box with nothing to scroll.
- Focus:
  - spatial moves around a grid of buttons, edges, and Next/Previous
    wrapping;
  - explicit targets, and disabled and hidden buttons skipped;
  - scroll-into-view.
- Bridge: key repeat timing, stick, d-pad, Shift+Tab, non-repeating
  accept; the modal context blocking a gameplay action.
- Text box: UTF-8 editing, selection replacement, deletes, control
  characters, commit, length limit, password, click and drag selection,
  long-text scrolling, and typing-aware keys.
- Dropdown:
  - opening below (or above near the bottom), with focus kept inside;
  - picking with keys or a click;
  - cancel and clicking outside.
- ListView: 5–6 row widgets for 1000 items, clicks, selection scrolling,
  activation and the wheel.
- Tooltips: delay, not catching clicks, hiding.
- Removed widgets forgotten safely.
- 368/368 tests pass on GCC 13, Clang and ASan/UBSan, and 400/400 with
  physics.

**Step 3: themes, layout files and data binding** (§18.3).

- **Themes** (`.atheme`):
  - A colour palette (names, hex or arrays).
  - Styles per control type and class (`Button.Primary`) that extend each
    other.
  - Text styles, and 9-slice brushes whose images resolve to textures.
  - Applied to a whole tree at once.
- **Layout files** (`.aui`):
  - The widget tree with every setting, only non-default slot fields,
    and its bindings.
  - Custom widget types by registration.
  - Errors that name the path to the bad widget.
- **Data binding**:
  - Widget text, visibility, enabled, opacity, progress, slider values,
    checks and selections follow reflected fields by path, through
    nested structs.
  - Ratios, formats, precision and inversion.
  - Two-way for the editable controls.
  - Only changed values are pushed, and text being typed is left alone.

**Verified**: 3 new tests.

- Themes:
  - hex, palette and array colours, and the bad ones;
  - brushes with resolved images, round-tripped;
  - a theme whose styles extend in either order, and lookup fallbacks;
  - save/load to the same file;
  - loops, unknown bases, missing colours and newer versions refused;
  - applied to buttons, text, progress bars and borders.
- Layouts:
  - a menu with every widget saved, loaded and saved again to the same
    text, with its bindings and resolved textures;
  - no default slots written;
  - nine kinds of error with paths;
  - a registered custom widget.
- Binding:
  - names, formatted numbers, precision, bools, ratios, inverted
    visibility;
  - updates only on change;
  - two-way slider, toggle, dropdown and typing;
  - text held while focused;
  - six kinds of bad binding reported;
  - direct path reads and writes;
  - a removed source.
- 371/371 tests pass on GCC 13, Clang and ASan/UBSan, and 403/403 with
  physics.

**Step 4: Widget Blueprints and UI animations** (§18.4).

- **Render transforms**: offset, scale and pivot per widget, for drawing
  and hit testing, without moving the layout.
- **UI animations**:
  - Keyframe timelines of opacity, offset, scale, tint, slot position
    and size, slider values and progress.
  - Six eases, speed, loops, reverse and start time.
  - One-property tweens that replace each other.
  - Saved in `.aui` files.
- **Widget components**: an entity's `WidgetComponent` shows a layout,
  bound to its own components.
  - Control changes become events, dispatched to Blueprints as
    `Event.OnWidgetClicked`, `OnWidgetValueChanged` and the rest.
  - A `UI` Blueprint library to create and remove widgets, set text,
    visibility, values and focus, and play animations.

**Verified**: 2 new tests.

- Transforms scale about the pivot and offset, for drawing and hits,
  with the layout unchanged.
- Eases, keyed tracks, loops, reverse, speed, tweens, validation, and
  animations round-tripped through a layout file.
- A widget entity bound to its own component; a click reaching a
  compiled Blueprint that creates a widget, prints the button's name,
  sets text and plays an animation.
- The `UI` library directly, and hiding, removing, switching and missing
  layouts.
- 373/373 tests pass on GCC 13, Clang and ASan/UBSan, and 405/405 with
  physics.

**Step 5: distance-field text and world-space UI** (§18.5).

- **SDF fonts** (`aether/ui/font.h`):
  - TrueType/OpenType through stb_truetype.
  - Glyphs rasterized on first use into a growing distance-field atlas,
    with dirty rectangles for upload.
  - Kerning, fallback glyphs and prewarming.
  - Text stays sharp at any size and zoom.
- **Text effects**: outlines and soft drop shadows, from the same atlas.
  They are set per Text widget or theme text style, and saved.
- **Font libraries**: Text widgets and text styles pick fonts by name.
- **World widgets** (`aether/ui/world_ui.h`): a layout at an entity's
  place, bound to its components.
  - Either projected onto the screen (sized by distance, pixel-snapped),
    or a quad in the scene that faces the camera or the entity's way.
  - Hidden when behind the camera, too far or off screen.
  - Picking by rectangle or ray.
  - Interactive ones take pointer input.
  - Their events reach Blueprints, and the UI library works on them.
- A Roboto font (Apache 2.0) in `assets/fonts` for the tests.

**Verified**: 2 new tests.

- Fonts:
  - loading, and bad files and settings refused;
  - metrics that scale with size;
  - glyph caching and dirty rectangles;
  - distances inside and outside a glyph;
  - kerning and fallbacks;
  - atlas growth that keeps glyphs, and overflow reported.
- Drawing:
  - coverage maths, and field spans through transforms and scaling;
  - an 'I' at 16 and 200 pixels, with edges at most two pixels wide and
    stems in proportion;
  - outline and soft-shadow passes checked pixel by pixel;
  - bitmap outlines.
- Fonts by name in viewports, layouts and themes, round-tripped, with
  bad effects refused.
- World widgets:
  - projection to the exact pixel;
  - bindings, clipping, and far-first order;
  - distance scaling and its limit;
  - hiding in four ways, and the UI scale;
  - quads with their corners and transform, billboarded and fixed;
  - picking and clicks on both kinds, drags off that don't click, and
    click-through;
  - the UI library;
  - problems reported once, and removal.
- 375/375 tests pass on GCC 13, Clang and ASan/UBSan, and 407/407 with
  physics.

**Step 6: the UI Designer** (§18.6, `editor/src/uidesign/`).

- **The layout document**:
  - Undoable tree edits: add, delete, duplicate, move and rename, with
    unique names that bindings and animation tracks follow.
  - Any widget's properties edited through its saved form, so custom
    types work.
  - Canvas placement and anchor changes that keep widgets in place.
  - Bindings, animation tracks and keys.
  - Eight checks (UD001–UD008).
- **The designer panel**:
  - A palette and a hierarchy with drag and drop.
  - A canvas at desktop, 4K, tablet and phone resolutions with safe
    areas, with pan and zoom, selection, move and resize handles, grid
    snapping and anchors shown.
  - Details with every property, the slot, anchor presets and bindings.
  - An animation timeline: key the current value, drag keys, scrub, and
    play on a preview copy.
  - Diagnostics and keyboard shortcuts.
- Like the other editors, it's hooked into the editor window with the
  Windows build.

**Verified**: 2 new tests.

- The document:
  - adding where widgets fit, and undo;
  - renames followed by bindings and tracks;
  - generic properties, including a Button keeping its label, and bad
    values refused;
  - Canvas placement, anchors kept across resolutions, stretched margins
    and merged drags;
  - moves, duplicates and deletes;
  - keys ordered, replaced and moved;
  - all eight diagnostics;
  - files round-tripped.
- The designer, drawn headless:
  - placing with snapping;
  - selecting, Ctrl+selecting, dragging and resizing with the mouse, each
    as one undo step;
  - nudging, and placing into containers;
  - phone safe areas and 4K;
  - themes on the preview only;
  - keying, scrubbing and looping playback;
  - Delete and Ctrl+Z keys, and selections that go stale.
- 377/377 tests pass on GCC 13, Clang and ASan/UBSan, and 409/409 with
  physics.

### Phase 19 (in progress) — VFX and particles

The `Aether::VFX` library (`vfx/`): particle systems as stacks of modules,
simulated on the CPU (the GPU path generates the same stack as a compute
shader later). Spec: [PHASE_SPECS.md, Phase 19](docs/design/PHASE_SPECS.md).

**Step 1: emitters, modules and the CPU simulation** (§19.1).

- **Values**:
  - Ranges.
  - Float curves: linear, smooth or stepped.
  - Colour gradients.
  - A seedable random generator, and curl noise.
- **Modules**:
  - Spawn: rate, bursts, and per distance moved.
  - Initialize: lifetime, seven shapes, cone, radial or direction
    velocity, size, colour, rotation and spin, and the emitter's own
    velocity.
  - Update:
    - gravity, drag, curl noise, vortices and attractors;
    - collision planes and kill volumes;
    - speed, size and colour over life.
- **Emitters**: world or local simulation, loops, delays, warmup and
  caps.
- **Assets**: saved in `.avfx` files, with errors that say where and
  seven checks (FX001–FX007).
- **The simulation**:
  - One array per attribute.
  - Births placed along the emitter's motion within the frame.
  - Repeatable for a given seed.
  - Whole systems of emitters.

**Verified**: 3 new tests.

- Values:
  - curves (keys, holds, smoothing, clamping);
  - gradients;
  - the generator's repeatability and distributions;
  - noise smoothness and a curl with no divergence;
  - JSON and its errors.
- Assets:
  - every module round-tripped;
  - eight kinds of load error with their paths;
  - all seven checks.
- The simulation:
  - rates, including two added;
  - lifetimes;
  - bursts in cycles and loops, start delays and the cap;
  - repeatability and Restart;
  - gravity and drag against their formulas;
  - all seven shapes;
  - velocities in a turned emitter, and radial velocity;
  - world against local space;
  - trails along the path, with inherited velocity;
  - vortex, attractor, kill volumes and a bouncing ball on a plane;
  - over-life curves;
  - warmup, stopping, bounds and systems.
- 50,000 particles with gravity, drag, size, colour and a plane take
  about 1.3 ms a frame.
- 380/380 tests pass on GCC 13, Clang and ASan/UBSan, and 412/412 with
  physics.

**Step 2: rendering data** (§19.2, `aether/vfx/render.h`).

- **Render modules**: a fourth stage on each emitter.
  - Sprites in five facing modes (billboard, towards the camera,
    stretched along velocity, about a fixed axis, flat in a plane), with
    rotation, aspect, blend modes, soft-particle fade and camera offset.
  - Flipbooks by life, rate or random frame, with frame blending.
  - Mesh particles, oriented by rotation, velocity or the camera.
  - Ribbons through the particles in birth order, stretched or tiled
    UVs, joined to the emitter if wanted.
  - Particle lights on every nth particle, up to a cap.
- **Sorting**: back to front, front to back, oldest or newest first.
- **Draw data**: world-space sprite, mesh, ribbon and light batches for
  the renderer's particle pass (GPU drawing with the Windows renderer).
- Three more checks (FX008–FX010).

**Verified**: 2 new tests.

- The camera from a view matrix.
- Flipbook cells and frames: over life, cycles, rate, random, a frame
  limit, and holding the last frame.
- Sprites:
  - batch settings and colours;
  - all four sort orders;
  - rotation and aspect;
  - every facing mode checked by its axes;
  - local-space emitters and camera offset;
  - blended flipbook frames;
  - expansion to triangles;
  - nothing drawn when disabled or empty.
- Meshes: transforms for each orientation.
- Ribbons:
  - vertex order, width and spread across the view;
  - stretched and tiled UVs, a fixed axis and joining the emitter;
  - no strip from one point.
- Lights: every nth, caps, radius, colour and intensity.
- A whole system's draw data.
- 382/382 tests pass on GCC 13, Clang and ASan/UBSan, and 414/414 with
  physics.

**Step 3: events, sub-emitters, parameters and scene collision** (§19.3).

- **Events**: birth, death (old age or killed) and collision (with the
  surface normal), in world space, recorded on request.
- **Sub-emitters**: another emitter of the system emits where a particle
  was born, died or hit.
  - Counts, probabilities and inherited velocity, colour and size.
  - Chains within a frame, and per-frame caps that stop loops.
- **Parameters**: floats, vectors and colours declared on the system and
  bound to module fields by path (`spawn[0].rate`), set at runtime.
- **Scene collision**: particle moves as rays against a collider, either
  any raycast (such as physics) or a depth buffer as the GPU does.
- Four system checks (FX011–FX014), and all of it in `.avfx` files.

**Verified**: 3 new tests.

- Events:
  - births and deaths in world space;
  - kills against old age;
  - collisions with their normals;
  - masks.
- `EmitAt`: overrides, and local space.
- Sub-emitters:
  - a firework bursting into sparks at the rocket's death, in its
    colour;
  - a system that finishes when its sparks are gone;
  - caps, probabilities, three-level chains, and a loop that stops;
  - splashes at collision points.
- Parameters:
  - rate, wind, tint and size bound and changed at runtime;
  - type and name errors;
  - six kinds of bad field path.
- The system's checks and files, with their errors.
- Scene collision:
  - a raycast ground the ball stays on and bounces off;
  - falling through with no collider;
  - a depth buffer drawn from a camera (hits, normals, behind, off
    screen);
  - a system's collider.
- 385/385 tests pass on GCC 13, Clang and ASan/UBSan, and 417/417 with
  physics.

**Step 4: the ParticleSystem component and Blueprint nodes** (§19.4).

- **`ParticleSystem` component**:
  - Settings: an asset, auto-activate, time scale, cull and LOD
    distances, and destroy-when-finished.
  - Blueprint methods: Activate, Deactivate, Restart, parameter
    setters, SetAsset and IsActive.
- **`ParticleWorld`**:
  - Pooled instances that follow their entities.
  - Distance culling (paused and hidden) and LOD (less spawning).
  - Finished events for Blueprints (`Event.OnParticleSystemFinished`)
    and one-shots that remove themselves.
  - Draw data for what's visible, skipping what's off screen.
- **`Particles` library**: Spawn Emitter at Location and Spawn Emitter
  Attached, which follows its target and ends when the target goes.

**Verified**: 3 new tests.

- Components:
  - auto activation, following the entity, and parameters;
  - problems reported once;
  - Deactivate and Activate, a system that waits to be activated, and
    Restart.
- One-shots:
  - finish once and remove their entity;
  - return to the pool and are reused.
- Rotation from degrees.
- Attached effects:
  - follow a moving, turning target;
  - end after the target goes.
- New assets, removed components, missing assets and transforms.
- Culling and LOD by camera distance, and paused culled systems.
- Draw data, and off-screen skipping by the view-projection.
- Time scale.
- A Blueprint that activates, spawns and tints an effect and prints when
  it finishes.
- 388/388 tests pass on GCC 13, Clang and ASan/UBSan, and 420/420 with
  physics.

**Step 5: the GPU path** (§19.5, `aether/vfx/gpu.h`).

- **CPU or GPU per emitter**: Auto picks the GPU for big emitters it
  can run. Sub-emitters, lights and ribbons need the CPU, and the check
  says which.
- **A generated HLSL compute shader** per emitter stack:
  - reset, emit and update kernels with dead and alive lists;
  - the whole Initialize and Update stack, with curl noise and
    depth-buffer collision when they're used;
  - enums compiled in, values read from constants.
- **Constants packed on the CPU** each frame, so parameters work without
  recompiling. Curves and gradients become lookup tables.
- **A CPU-side driver** keeps the clock and spawn modules and hands the
  GPU its spawn counts and frame constants. Dispatch comes with the
  Windows renderer.

**Verified**: 3 new tests.

- What can run on the GPU, and the target choice in every setting, saved
  and loaded.
- Generated shaders:
  - the right kernels and resources;
  - constants counted, ordered and named;
  - the same key when only values change, and a new one when the stack
    changes;
  - disabled modules left out.
- 14 variants (every shape, velocity mode, space, volume shape and
  module frame, plus an empty stack) compiled with glslangValidator,
  every kernel, and checked with spirv-val.
- Baked curves within 0.03 of the curve, and gradient tables.
- The frame constants' layout, word by word.
- The driver:
  - spawn counts from rates and bursts;
  - frame seeds;
  - motion and velocity;
  - parameters in the spawner and the constants;
  - stop and restart.
- 391/391 tests pass on GCC 13, Clang and ASan/UBSan, and 423/423 with
  physics.

**Step 6: the particle editor** (§19.6, `editor/src/vfx/`).

- **The document**: undoable edits to emitters, settings, modules and
  their fields, parameters, bindings and sub-emitters.
  - Renames and moves keep references intact: sub-emitters follow
    emitter names, and bindings follow module moves and parameter
    renames.
- **The editor**:
  - Emitter list and module stack, with add menus, enabling, reordering
    and removal.
  - Details with curve and gradient editors, min/max ranges, enum
    combos, and the emitter's settings, sub-emitters and bindings.
  - A live preview: orbit camera, play/pause/speed/loop, and a timeline
    scrubber that replays deterministically.
  - Parameters, per-emitter stats (particles, spawned, bounds, CPU time,
    CPU or GPU) and diagnostics.
- Like the other editors, it's hooked into the editor window with the
  Windows build.

**Verified**: 2 new tests.

- The document:
  - emitters added, duplicated, renamed with sub-emitters following,
    moved, removed and undone;
  - settings, including refused ones;
  - modules by stage, fields and refusals, enabling, and merged drags;
  - bindings following module moves and removals and parameter renames;
  - sub-emitters with checks;
  - files.
- Curve and gradient keys: ordering, replacing, clamping and removal
  limits.
- The preview:
  - whole steps, and seeking that matches playing exactly;
  - the seek limit;
  - stats;
  - the camera and projection.
- The panel, drawn headless:
  - playing and pausing;
  - scrubbing;
  - an edit shown at the same moment;
  - details for every kind of field;
  - Delete and Ctrl+Z keys;
  - stale selections dropped;
  - looping.
- 393/393 tests pass on GCC 13, Clang and ASan/UBSan, and 425/425 with
  physics.

### Phase 20 — AI and navigation

The `Aether::Nav` library (`nav/`) bakes navigation meshes with Recast and
queries them with Detour (recastnavigation 1.6, fetched by CMake). Crowds,
Behavior Trees and perception come next. Spec:
[PHASE_SPECS.md, Phase 20](docs/design/PHASE_SPECS.md).

**Step 1: navmesh baking and path queries** (§20.1).

- **Geometry**: triangles with an area each (0 blocked, 63 ground, 1–62
  your own), with plane and box helpers.
- **Settings** per agent:
  - voxel sizes;
  - the agent's height, radius, climb and slope;
  - region, edge and detail settings;
  - the tile size.
- **Baking**:
  - square tiles, each baked apart with a border;
  - the full Recast pipeline, keeping areas;
  - stats, and errors that say why;
  - `BuildNavTile` rebakes a single tile.
- **Files**: `.anav` files (Detour tile data plus the grid and
  settings).
- **Queries**:
  - paths that are Complete or Partial, as corners;
  - nearest points, with their area;
  - raycasts over the mesh, giving the hit and the wall's normal;
  - seeded random points (anywhere, or within a radius over connected
    ground);
  - reachability;
  - swapping or removing a tile;
  - polygons for drawing.
- **Filters**: a cost per area, and excluded areas that are never
  entered. Exclusion uses a Detour filter subclass, so Detour is built
  with its virtual query filter.

**Verified**: 11 new tests.

- A straight path on a floor.
- Bad input refused.
- Paths around a wall.
- Raycasts: stopped by a wall, clear alongside it, and at the floor's
  edge.
- A climbable step, and a block too high to climb, whose top gives a
  Partial path.
- The agent's radius keeping the mesh off edges, and closing a narrow
  corridor.
- Area costs choosing between wading and the bridge, and exclusion.
- `.anav` round trips, and broken files refused.
- Tiled meshes matching a single tile.
- Random points: repeatable, and staying within their radius.
- Rebaking one tile for a crate, saved with the mesh, and removing a
  tile.
- 404/404 tests pass on GCC 13, Clang and ASan/UBSan, and 436/436 with
  physics.

**Step 2: obstacles, areas, links and the scene** (§20.2).

- **Volumes**: a box turned by a yaw, a cylinder, or a convex prism.
  - Area 0 carves a hole, and the agent's radius is kept clear of it.
  - Any other area relabels the ground inside.
- **Off-mesh links**:
  - jumps, ladders and drops, both ways or one way;
  - each has an area (for costs and exclusion) and a user id;
  - path points flag where a link starts;
  - the baked links can be listed.
- Both are baked in from `NavGeometry`, or changed at runtime with
  `DynamicNavMesh`:
  - adding, updating or removing marks only the tiles touched dirty;
  - `Update` rebakes a budget of tiles per frame;
  - a revision counter tells agents to replan;
  - it can start from a loaded `.anav`.
- **Components**:
  - `NavObstacle`: a box or cylinder that carves with its entity, after
    a move threshold or a 5° turn.
  - `NavModifierVolume`: an area box.
  - `NavLinkProxy`: a link in the entity's space.
- **`NavWorld`**:
  - gathers static geometry from models, placed by world pose;
  - bakes or loads the mesh;
  - follows the components as entities are added, moved, edited or
    destroyed.

**Verified**: 8 new tests.

- An obstacle carving, keeping the agent's radius off it, moving and
  being removed; unchanged updates are free.
- Rebakes spread one tile per frame, and marking everything dirty.
- Cylinders, turned boxes and prisms.
- Baked and runtime area volumes (water, a road, a blocker).
- Links:
  - baked in, both ways, with path flags, listing and area costs;
  - a one-way drop added and removed at runtime.
- Starting from a loaded `.anav`, saving rebakes, and resetting.
- The components:
  - the crate left out of the geometry;
  - move thresholds and turning;
  - `carve` off and on;
  - destroyed entities;
  - a pond.
- Placed pads, a turned link proxy that is enabled, disabled and made one
  way, and loading.
- 412/412 tests pass on GCC 13, Clang and ASan/UBSan, and 444/444 with
  physics.

**Step 3: agents and crowds** (§20.3).

- **`NavAgent`**, steered by Detour's crowd:
  - it walks to a point or follows an entity;
  - it avoids other agents and separates from them, and anticipates
    corners;
  - it turns to face where it goes;
  - it stops within a stopping distance, and sits a base offset above
    the mesh.
- **Methods** (Blueprint nodes and Luau): `MoveTo`, `MoveToEntity`,
  `Stop`, `Warp`, and status, velocity, speed, remaining-distance and
  goal queries.
- **Moves**:
  - a move fails when the goal is cut off or off the mesh, unless
    `allow_partial`;
  - each arrival or failure fires `Event.OnMoveCompleted`.
- **`NavCrowd`**:
  - adds and removes agents with their entities;
  - replans when obstacles change the mesh;
  - makes a new crowd after a rebake;
  - lists an agent's next corners.
- **`Navigation`** Blueprint library: reachability, path length,
  projecting onto the mesh, on-mesh tests, random reachable points and
  raycasts.

**Verified**: 11 new tests.

- Walking to a point: timing, facing, coming to rest, and the event.
- Going round a wall.
- Unreachable and off-mesh goals failing, and partial moves.
- Two agents passing each other.
- Following a moving entity, and one that goes.
- Stop and Warp, the base offset, and removal.
- Replanning round a new obstacle, and a walled-in goal failing then
  succeeding.
- Waiting for a mesh, and carrying on after a rebake.
- The `Navigation` library.
- Blueprint nodes (Random Reachable Point into Move To, then Event
  OnMoveCompleted).
- Luau calling the agent's methods.
- 423/423 tests pass on GCC 13, Clang and ASan/UBSan, and 455/455 with
  physics.

**Step 4: Blackboards and Behavior Trees** (§20.4), in the new
`Aether::AI` library (`ai/`).

- **Blackboards**: typed keys (Bool, Int, Float, String, Vector, Entity)
  with initial values, type checks, and revisions per key. Values set
  before the tree is known are kept when they fit its keys.
- **`.abt` assets** in JSON, with load errors that say where, and ten
  checks (BT001–BT010).
- **Composites**: Selector, Sequence, and Parallel (succeeding when all
  or one of its children do).
- **Decorators**:
  - Blackboard conditions that compare values and abort their own
    branch, lower priorities, or both;
  - Cooldown, Loop, TimeLimit, Inverter, ForceSuccess and ForceFailure.
- **Services**: Blueprint events, Luau functions, and DistanceTo, at
  intervals.
- **Tasks**:
  - Wait (with deviation) and MoveTo (drives the NavAgent, and follows
    a changing goal);
  - Set and Clear Blackboard;
  - Run Blueprint (ended by `Behavior Trees > Finish Task`) and Run
    Luau (ticked);
  - Log, Succeed and Fail.
- **`BehaviorTreeComponent`**:
  - Start, Stop and Restart;
  - blackboard getters and setters per type;
  - the active node;
  - a tick interval.
  - `BehaviorTreeWorld` runs these components. Debugger data: active
    paths and each node's last status.

**Verified**: 11 new tests.

- Blackboards: types, adoption, revisions and JSON values.
- Files: a round trip, load errors, and every check.
- Composites and waits.
- Conditions that abort: Self, LowerPriority and both, and comparisons.
- Cooldowns, loops, time limits, and inverting and forcing.
- Parallel, both ways.
- Blackboard tasks and the DistanceTo service.
- Blueprint and Luau hooks: waiting, finishing, aborts and services.
- The component and world: kept values, typed setters, Stop, Start and
  Restart, changing trees, tick intervals, and missing trees.
- MoveTo with a real NavAgent: arriving, a changing goal, and halting.
- Blueprint nodes (a Blueprint task finishing itself), and Luau tasks,
  services and methods.
- 435/435 tests pass on GCC 13, Clang and ASan/UBSan, and 467/467 with
  physics.

**Step 5: perception** (§20.5).

- **`AIPerception`**:
  - sight, with a notice radius and a larger keep-track radius, a view
    cone, eye and target heights, and line of sight (a callback, or
    over the navmesh);
  - hearing noises scaled by loudness;
  - learning attackers from damage;
  - teams;
  - forgetting after a while, or at once when the actor is destroyed.
- **Memory**: last known location, age, visibility, sense and strength
  per actor, and the target it's most aware of.
- **Other components**: `AIStimuliSource` (what can be seen), and the
  `Perception` Blueprint library (`ReportNoise`, `ReportDamage`).
- **Events**: `Event.OnTargetPerceived (actor, sense, sensed)` and
  `Event.OnTargetForgotten (actor)`.
- **Blackboard sync**: the target and its last location are written to
  the Behavior Tree's blackboard.

**Verified**: 7 new tests.

- Sight: radii, keeping track, the cone, turning, all-round vision,
  hidden sources, and not seeing itself.
- Line of sight, by callback and over the navmesh.
- Hearing (loudness, its own noises) and damage.
- Teams, forgetting, and destroyed actors.
- Which target it picks.
- A Behavior Tree switching between patrolling and chasing through the
  blackboard.
- Blueprint events and the Report Noise node.
- 442/442 tests pass on GCC 13, Clang and ASan/UBSan, and 474/474 with
  physics.

**Step 6: the editor** (§20.6).

- **Debug drawing**, as plain lines and triangles for the viewport:
  - the navmesh by area, with its edges and tile grid;
  - links as arcs;
  - obstacles and area volumes;
  - paths, and each agent's corners and goal;
  - sight cones, hearing ranges, and lines to what each AI knows.
- **The Behavior Tree document**:
  - add, move, duplicate and remove nodes, and change their type;
  - edit fields, decorators and services through the saved form;
  - blackboard keys whose renames follow every reference;
  - checks, undo and files.
- **The Behavior Tree editor**:
  - an automatic top-down layout with notes for decorators and
    services, and errors on nodes;
  - edits by wire (reparent), sideways drag (reorder), Delete and
    Ctrl+D;
  - a palette;
  - Blackboard, Details and Diagnostics panels;
  - a debugger: the active branch highlighted, results coloured, and
    live blackboard values.
- **The Navigation panel**: bake settings, Bake and Rebuild All, stats,
  and the overlay's toggles.

**Verified**: 7 new tests.

- Navigation debug drawing: areas, toggles, the tile grid, the three
  volume shapes, dynamic volumes, link arcs and paths.
- AI debug drawing: corners, goals, sight cones (and all-round),
  hearing, and what's seen.
- The document:
  - moves, including a later-sibling target, the root, and a node
    under itself;
  - duplicates, removal, and type changes;
  - fields through the saved form, with merged drags and refusals;
  - decorators, typed comparison values, and services;
  - keys: renames following references, type changes, initial values
    and descriptions, removal showing up in the checks;
  - undo and files.
- The layout, titles and notes, the view's pins, wires and errors, and
  the debugger's highlights.
- Graph edits: reparenting, refusals, reordering by drag, and Delete.
- The editor drawn headless: selection, the palette, dropped
  selections, the debugger, and Ctrl+Z.
- The Navigation panel: settings JSON, baking, the overlay, and a
  failed bake.
- 449/449 tests pass on GCC 13, Clang and ASan/UBSan, and 481/481 with
  physics.


### Phase 21 — World Building

Build spec: [docs/design/PHASE_SPECS.md](docs/design/PHASE_SPECS.md), Phase 21.

**Step 1: heightmap terrain** (`terrain/`).

- **Heightmap**: procedural Perlin noise generation (multi-octave), bilinear sampling.
- **TerrainData**: chunk layout from heightmap dimensions, LOD distances.
- **TerrainChunk**: one chunk per tile, LOD 0 built from heightmap; central-difference normals.
- **Vertex generation**: position (3) + normal (3) + UV (2) = 8 floats/vertex.
- **Index generation**: indexed triangle list for the RHI.
- **LOD selection**: distance-based level of detail switching.

**Step 2: splatmap material layers and terrain painting**.

- **SplatmapLayer**: name, albedo/normal textures, albedo tint, metallic, roughness, tile scale.
- **SplatmapData**: RGBA8 splatmap pixels (R=layer0, G=layer1, B=layer2, A=layer3).
- **Pack/unpack**: encode/decode weights to/from RGBA8 pixels.
- **Brush**: circular falloff, strength and radius; apply to splatmap or heightmap.
- **Paint modes**: raise/lower (default), flatten (lerp toward center height), smooth (neighbor average).
- **Layer normalization**: blend weights to sum to 1.0.

**Verified**: 13 new tests (7 terrain, 6 splat).

**Steps 3–5: terrain rendering, foliage and splines**.

- **Rendering**: chunks upload through a `TerrainGpu` interface (the
  renderer implements it over the RHI). Dirty chunks rebuild, LOD
  changes re-upload, and VRAM is estimated.
- **Foliage**:
  - generated from sample points through a density map, with random
    types, scale, rotation, normal alignment and anchor offsets;
  - painted over a brush disc at a density per m², and erased;
  - filtered by chunk and sorted by type.
- **Splines**: Catmull-Rom curves with width and roll, strip meshes,
  and dashed road markings.

The terrain module didn't compile when it first landed. It was repaired
in #96 and #97, which also fixed:
- every chunk using the first chunk's heights;
- lowering never working;
- noise that wasn't smooth and overflowed signed integers;
- the stub functions.

**Step 6: large worlds** (`streaming/`, §21.6).

- **Partition**: a level splits into grid-cell scenes plus a persistent
  scene (no Transform, AlwaysLoaded, streaming sources). Children go
  with their parents.
- **Index and files**: `world.aworld` (JSON) and binary cell scenes.
- **`WorldStreamer`**:
  - loads cells within each `StreamingSource`'s radius, nearest first;
  - unloads past radius × 1.25, so cells on a border don't thrash;
  - has load and unload budgets per update, and pins;
  - reports cells that fail;
  - adds GUIDs to the index, so parents resolve across cells.
- **Floating origin**: shifts root Transforms back near zero in whole
  steps and keeps a double-precision offset. The streamer works in
  whole-world positions through it.

**Verified**: 5 new tests.

- Cell coordinates and bounds.
- Partitioning: cells, children following parents, the persistent
  scene, the index round trip, and copying entities.
- Streaming around a moving player: hysteresis, destroyed and spawned
  entities, and GUIDs.
- Budgets with nearest-first loading, failures, and pins.
- Files (and replacing stale cells), and the floating origin with
  streaming.
- 497/497 tests pass on GCC 13, Clang and ASan/UBSan, and 529/529 with
  physics.

**Step 7: the world-building editors** (§21.7).

- **Terrain**:
  - raise, lower, smooth, flatten and paint;
  - strokes as single undo steps that store only the changed region;
  - dirty chunks for the renderer;
  - a cursor ring that follows the ground;
  - dabs spaced along a drag.
- **Foliage**:
  - paint and erase, with instances set onto the ground;
  - undo per stroke;
  - type editing, density, the instance cap, and counts.
- **Splines**:
  - add, insert, remove and drag points (each drag is one undo step);
  - width and roll;
  - picking, and the curve and road edges for the viewport.
- **World partition map**: cell states, pins, entity counts, and
  streaming sources with their reach.
- Like the other editors, they're hooked into the editor window with
  the Windows build.

**Verified**: 5 new tests.

- Terrain strokes:
  - one undo step per stroke, exact undo and redo;
  - dirty chunks, including both chunks on a shared edge;
  - lower, flatten and smooth;
  - strokes that change nothing aren't kept;
  - paint, and the cursor.
- Pointer-driven dab spacing.
- Foliage: on the ground with anchors, erase, undo, types, density and
  the cap.
- Splines: insert, drags as one step, width and roll, picking, the
  curve and road edges, selection, and the viewport gestures.
- Every panel drawn headless, and the partition map's hit testing and
  pins.
- 502/502 tests pass on GCC 13, Clang and ASan/UBSan, and 534/534 with
  physics.

### Phase 22 (done) — Networking and multiplayer

The `Aether::Net` library (`net/`). Spec:
[PHASE_SPECS.md, Phase 22](docs/design/PHASE_SPECS.md).

**Step 1: the transport** (§22.1). It's our own small protocol over a
datagram interface, so the same code runs over UDP and over a simulated
network.

- **Sockets**:
  - `UdpSocket`: non-blocking, POSIX or WinSock;
  - `LoopbackNetwork`: in memory, with seeded latency, jitter (so
    packets reorder), loss and duplication, and time you advance.
- **`Connection`**:
  - sequence numbers with 33 acks per packet;
  - reliable ordered messages, resent on an RTT-based timer and
    delivered once and in order;
  - fragments for large messages, up to 256 KB;
  - unreliable and sequenced channels;
  - bounded queues;
  - RTT and loss stats.
- **`NetHost`**:
  - listen, connect with retries, and refuse when full;
  - keepalives, timeouts and goodbyes;
  - protocol ids;
  - events for connect, disconnect (with a reason) and messages;
  - Send and Broadcast.

**Verified**: 8 new tests.

- Addresses and wrapping sequence numbers.
- The simulated network: latency, closed ports, and loss rates.
- Handshakes, messages both ways, and goodbyes.
- 500 reliable messages, all delivered once and in order, over 20%
  loss, 5% duplication and jitter; unreliable and sequenced delivery;
  resends, losses and RTT.
- A 100 KB message in fragments, and the limits.
- A full server refusing, and a missing server.
- Keepalives over 20 idle seconds, timeouts, and foreign protocols.
- Real UDP on localhost.
- 510/510 tests pass on GCC 13, Clang and ASan/UBSan, and 542/542 with
  physics.

**Step 2: replication** (§22.2). The server is authoritative and sends
delta snapshots in the manner of Quake 3.

- **`NetIdentity`**: a net id, owner, archetype, relevancy radius and
  priority.
- **What is sent**: `Field_Replicated` fields (and all of `Transform`),
  per component, with masks of the fields that changed since the last
  snapshot the client acknowledged. An idle world costs a 17-byte
  header, and a lost snapshot costs only latency.
- **Spawns and despawns**: spawns carry everything, and an `OnSpawn`
  hook per archetype adds what isn't replicated. Despawns are explicit.
- **Ownership**: `locally_owned` on the client.
- **Relevancy**: by distance from the client's viewer (its pawn, or one
  set with `SetViewer`).
- **Bandwidth**: a byte budget per snapshot, shared by accumulated
  priority.

**Verified**: 5 new tests.

- Spawns (with hooks), updates (positions, rotations, strings, and
  non-replicated fields left alone), removed components and despawns.
- Header-only idle snapshots, one small entry for one move, and only
  changed fields written.
- Convergence through 30% loss, jitter and duplication, with spawns and
  despawns along the way.
- Relevancy as viewers move, ownership and its handover, and
  disconnects.
- The budget kept, everything arriving in the end, and a high-priority
  entity keeping up.
- 515/515 tests pass on GCC 13, Clang and ASan/UBSan, and 547/547 with
  physics.

**Step 3: remote calls** (§22.3). These are RPCs in the manner of
Unreal.

- **Flags**: `Fn_Server`, `Fn_Client`, `Fn_Multicast` and
  `Fn_Unreliable` on reflected functions.
- **Every call goes through `CallFunction`**: this is how C++ calls
  them, and the Blueprint VM and Luau now do too. The world's router
  then runs the call, sends it, or refuses it.
- **`net::RpcRouter`**:
  - server calls are sent only by their owner and run only from it;
  - client calls go to the owner;
  - multicasts go to every client that has the entity.
  Without a router, everything runs locally (single player).

**Verified**: 5 new tests.

- Server calls from the owner, refused for others, rejected from a
  client that lies about owning the entity, and following an ownership
  handover; unreliable calls; mismatched arguments.
- Client calls to the owner only; multicasts limited by relevancy; local
  calls on clients; non-remote functions.
- Single-player worlds, the router's lifetime, and malformed or unknown
  calls.
- A Blueprint, and a Luau script, calling through the network: the
  owner's calls get through, and others get BP204 or a script error.
- 520/520 tests pass on GCC 13, Clang and ASan/UBSan, and 552/552 with
  physics.

**Step 4: prediction and interpolation** (§22.4).

- **Snapshot interpolation**: other machines' entities are shown 0.1 s
  in the past, between two snapshots, against a smoothed server clock.
- **Predicted character movement**:
  - **`NetMovement`** and a deterministic `StepMovement` (which can be
    replaced).
  - **The owner** moves at once and sends its inputs redundantly.
  - **The server** runs inputs only from the owner, once each and
    sanitized, and acknowledges them.
  - **Reconciliation**: the owner rewinds to the acknowledged state and
    replays the inputs since.

**Verified**: 5 new tests.

- Buffer sampling: lerp, the shorter-arc nlerp, clamping and capacity.
- Interpolated motion every frame (not 0.25 m snapshot steps), with a
  steady lag and no starvation.
- Prediction that responds the same frame, jumps, and ends exactly where
  the server does with no corrections; other clients agree.
- 20% loss with jitter, covered by the input redundancy, and a
  server-side knockback reconciled.
- The movement rule, sanitized speed hacks that replay identically, and
  inputs from someone else rejected.
- 525/525 tests pass on GCC 13, Clang and ASan/UBSan, and 557/557 with
  physics.

**Step 5: sessions** (§22.5).

- **The simulated network gains hosts**: virtual machines (10.0.0.x)
  and broadcast, and UDP sockets can broadcast.
- **`SessionInfo`**: name, map, mode, port, players, build, a password
  flag and properties.
- **LAN discovery**: `LanBeacon` answers broadcast queries; `LanBrowser`
  collects sessions with their ping and build compatibility, and expires
  them.
- **Hosting and joining**: `SessionHost` admits players by build,
  password and room, drops silent peers, keeps names unique, and can
  kick. `SessionClient` ends Joined or Failed with a reason.
- **`LobbyService`**: an interface for platform services later, with a
  LAN implementation now.

**Verified**: 5 new tests.

- Simulated hosts, ports and broadcast delivery.
- Session info round trips, cut-short data, and long strings.
- Discovering two servers (one with another build), updated player
  counts, other protocols ignored, expiry, auto-search, and disabled
  beacons.
- Joining with the right password; refused for a wrong password, a wrong
  build, or a full session; duplicate names, silent peers dropped,
  leaving, kicking, and no server at all.
- The LAN lobby service advertising, finding and stopping.
- 530/530 tests pass on GCC 13, Clang and ASan/UBSan, and 562/562 with
  physics.

**Step 6: the editor** (§22.6).

- **Networked Play-in-Editor** (`NetPlaySession`): a listen or dedicated
  server and N clients, in one process over a simulated network whose
  latency, jitter, loss and duplication change live. The edited world
  is copied, never touched. Player pawns are predicted; everything else
  is interpolated.
- **The network profiler**: bytes per entity, component and field, as
  totals and rates.
- **Panels**: Net Play (mode, clients, network sliders, per-client RTT,
  loss, traffic and corrections) and Net Profiler (by entity, by field).

**Verified**: 4 new tests.

- A listen server with 2 clients: the level arrives with its models;
  predicted walking agrees with the server; the listen player is seen
  by clients; the edited world is untouched.
- A dedicated server with 3 clients converging after the network turns
  bad mid-play, and a clean restart.
- The profiler ranks the moving player first, with Transform.position
  its costliest field, and still scenery costs nothing.
- Both panels drawn headless before, during and after play.
- 534/534 tests pass on GCC 13, Clang and ASan/UBSan, and 566/566 with
  physics.

### Phase 23 (done) — Profiling, debugging and developer tools

Spec: [PHASE_SPECS.md, Phase 23](docs/design/PHASE_SPECS.md).

**Step 1: console variables and the console** (§23.1).

- **CVars** are typed settings declared where they're used
  (`AutoCVar<int> cascades("r.shadows.cascades", 4, ...)`).
  - They parse, clamp to a range, and can be reset to their defaults.
  - Flags: read-only, cheat, archive and requires-restart.
  - Change callbacks; reads are thread-safe.
- **The console** runs `name`, `name value` and `command args`, with
  `;`, quotes and comments.
  - Built-in commands: help, find, cvarlist, set, reset, toggle, echo,
    exec, writeconfig, history and clear.
  - It has history and Tab completion, reads and writes config files,
    and takes `+name value` on the command line.
  - It can capture the log from any thread.
- **The logger** gains sinks and a buffer of recent lines.
- **The console panel**: a filtered, coloured output pane and an input
  line with history and completion. It doubles as an in-game overlay
  toggled with the tilde key.

**Verified**: 7 new tests.

- CVar parsing, ranges, defaults, callbacks, registry rules, and
  variables and commands declared where they're used.
- Statements, quotes, comments, read-only and cheat refusals,
  suggestions, and every built-in command.
- Completion, history, config files written and read back, nested exec
  limits, and the command line.
- Log sinks, the recent-lines buffer, and capture from another thread.
- The panel's history and Tab completion, and drawing docked and as an
  overlay.
- 541/541 tests pass on GCC 13.

**Step 2: the profiler** (§23.2).

- **Zones**: `AETHER_PROFILE_ZONE` records per thread without a lock.
  Frames gather zones, per-frame counters, gauges and GPU pass times;
  the last 300 frames are kept.
- **Statistics**: total, self and max time per zone.
- **Export**: a Chrome or Perfetto trace.
- **Tracy**: optional forwarding (`-DAETHER_TRACY=ON`).
- **Instrumented**: scheduler phases and every system; job workers are
  named.
- **Memory by category**: the allocators report their usage.
- **Console**: profiler variables and commands.
- **The profiler panel**: a frame graph (click a frame to inspect it),
  zone tables, a per-thread timeline, counters and GPU passes, memory,
  and export.

**Verified**: 4 new tests.

- Nesting and self time, counters, gauges and GPU timings, and the
  enabled, paused and history settings.
- Named threads, and the scheduler's and workers' zones.
- The Chrome trace's contents, the allocators' memory, and the console
  commands.
- The panel's frame selection, zone statistics per frame and across
  all frames, export, and drawing.
- 545/545 tests pass on GCC 13.

**Step 3: debug drawing and stat overlays** (§23.3).

- **Debug drawing**:
  - lines, arrows, boxes, spheres, circles, points, axes and text, in
    the world or on screen;
  - for one frame or for a duration;
  - from any thread, with a limit, and switched off with the
    `debug.draw` CVar;
  - callable from C++, from Blueprints (the `DebugDraw` nodes) and from
    Luau (`Draw.line`, `Draw.box`, ...).
- **Stat overlays**:
  - groups: `stat fps`, `zones`, `counters`, `gpu` and `memory`;
  - `stat net` for a network host;
  - custom groups;
  - the `stat` command.
- **The debug overlay**: draws all of it over a viewport with ImGui.

**Verified**: 6 new tests.

- The lines each shape becomes, rotations and colors.
- Durations and ticking, screen text, the CVar, the limit, and drawing
  from four threads.
- The reflected functions, and a Blueprint drawing a sphere and text.
- The Luau `Draw` table and its errors.
- Built-in, custom and net stat groups, and the `stat` command.
- The overlay's projection, near-plane clipping, text and stats, drawn
  headless.
- 551/551 tests pass on GCC 13.

**Step 4: crash handling** (§23.4).

- **What is caught**: fatal signals (on an alternate stack), Windows SEH
  exceptions (with a minidump), and uncaught C++ exceptions.
- **The report file** holds the reason, build, address, thread, the
  game's context, a backtrace and the last 200 log lines.
- **Signal safety**: the handler writes it with async-signal-safe calls
  only, then lets the process die as it would have.
- **Next run**: the reports are listed, read, dismissed or deleted. The
  editor's crash reporter shows them at startup.

**Verified**: 3 new tests.

- A report written on purpose and read back: fields, context, log and
  backtrace; then listing, dismissing and deleting.
- Real crashes in child processes: a segfault, an abort, an uncaught
  exception and SIGFPE. Each is reported once, and the process still
  dies of it.
- The reporter dialog opening, its report text, dismissing and
  deleting, and drawing headless.
- 554/554 tests pass on GCC 13.

**Step 5: functional tests** (§23.5).

- **Scenarios** run headless with a fixed step, built from steps:
  - load a scene;
  - set up;
  - simulate, or simulate until a condition holds;
  - check;
  - "reaches": an entity with a tag reaches a box in time.
- **Failures**: the first failing step ends the test, and the result
  says why and where. Log lines are captured.
- **Registry**: tests register with `AETHER_FUNCTIONAL_TEST` and are
  filtered by name or tag.
- **Reports**: JUnit XML (for CI), JSON, and a summary.
- **`aether_functional`**: a command-line runner, registered with CTest,
  with sample scenarios.
- **Console**: the `functional.run` and `functional.list` commands.
- **A fix**: the profiler's worker-thread test no longer depends on how
  the scheduler happens to split jobs (it failed once under ASan).

**Verified**: 3 new tests, and the runner's 2 sample scenarios.

- Steps in order, fixed-step simulation, "reaches", and the log.
- Every kind of failure and its explanation.
- Scenes from files, the registry's filters, tags and stop-on-failure,
  the JUnit and JSON reports, and the console commands.
- 557/557 tests pass on GCC 13, and `aether_functional` passes 2/2.

### Phase 24 — Cross-platform

Spec: [PHASE_SPECS.md, Phase 24](docs/design/PHASE_SPECS.md).

**Step 1: continuous integration** (§24.1).

- **GitHub Actions on every push and pull request**:
  - Linux: GCC, Clang, ASan+UBSan and physics builds, each running the
    unit and functional tests.
  - Windows: an MSVC build of everything, the D3D12 editor included.
- **The packaged editor**: the Windows job packages
  `AetherEditor-windows-x64.zip` (`aether_editor.exe`, its DLLs and the
  assets) as a download on each run.
- **Releases**: a `v*` tag publishes the zip as a GitHub release.
- **Assets**: the editor finds `assets/` next to its executable, so the
  zip runs anywhere.

**Step 3: a portable window and input layer** (§24.2). `platform::Window`
runs on Win32 or GLFW (X11, Wayland, Cocoa) and reports the same events
everywhere - keys, mouse, wheel, characters, gamepads, focus, resizes,
close - which `ApplyWindowEvents` feeds into the input state. Linux CI
runs the window tests under Xvfb.

**Step 4: the Vulkan backend off Windows** (§24.3).

- **Builds on Linux**: the RHI's Vulkan backend now builds there, with
  glslang compiling the engine's HLSL to SPIR-V.
- **Window swap chains**: these work through GLFW.
- **Offscreen swap chains**: these render with no window, and
  `ISwapChain::ReadBack` copies a frame back to memory.
- **Tests on lavapipe**: Linux CI renders the tests with Mesa's software
  Vulkan driver, with no GPU needed.

**Step 5: the editor on Vulkan** (§24.4). `aether_editor_shell` runs the
editor's panels on any platform with Vulkan, in a GLFW window or
offscreen:

- **Panels**: every tool editor (see "Every tool in both editors" below).
- **Rendering**: Dear ImGui is drawn through the engine's own Vulkan
  device.
- **Input**: comes from the engine's portable window events.
- **CI**: Linux CI screenshots the shell on lavapipe.

**Step 6: macOS, ARM and platform plugins** (§24.5).

- **macOS**: the engine builds on Apple Silicon. Its SIMD math compiles
  to NEON through sse2neon, and Vulkan runs on Metal through MoltenVK.
  CI builds and tests it.
- **Platform plugins**: Android and consoles plug in from outside the
  engine, with startup and shutdown hooks, a user data directory and RHI
  device factories (see `platforms/README.md`).

### Phase 25 (in progress) — Build, cook and package

Spec: [PHASE_SPECS.md, Phase 25](docs/design/PHASE_SPECS.md).

**Step 1: .apak archives and the virtual file system** (§25.1).

- **Archives**: entries compressed with LZ4 or zstd (or stored raw when
  that's smaller), each with a CRC-32, and an index at the end.
  Truncated and damaged archives are detected.
- **Virtual file system**: mounts directories and archives by priority,
  so patch and DLC paks override the base and loose files override
  both.
- **`aether_pak`**: creates, lists, extracts and verifies archives.

**Step 2: the cooker** (§25.2). `aether_cook <project> --out <dir>`:

- **What it cooks**: what the startup scene reaches, plus the
  `always_cook` content, skipping everything else.
- **What it strips**: editor-only fields (`Field_EditorOnly`).
- **What it writes**: the assets, their imported data and a manifest,
  all into one `.apak`.

**Step 3: cooked textures** (§25.3). The cooker block-compresses
textures (BC1, BC3, BC4, BC5, BC7) with sRGB-correct mips. Normal maps
use BC5, with renormalized mips. The results are stored in `.atex`
files that GPUs can sample directly.

**Step 4: the player** (§25.4). `aether_player` runs a cooked game
without the editor:

- **Loading**: it mounts the game's `.apak` archives (later ones patch
  earlier ones) and reads the manifest. It loads the startup scene, with
  its prefabs, from the archives.
- **Running**: the scene runs on the fixed-timestep frame loop, with
  lifecycle callbacks, and with physics when the engine has it. It runs in
  a window or headless.
- **Configurations**: Debug, Development and Shipping set the logging.
- **Not yet**: drawing the scene. The window shows a clear colour until
  the renderer's RHI path lands.

```bash
aether_cook MyGame.aproject --out Build/Paks --config shipping
aether_player --pak Build/Paks          # or put the paks in Paks/ beside it
```

**Step 5: packaging from the editor** (§25.5). The editor's **Build and
Package** window cooks the project, or packages it, on a background
thread, with a progress bar and a log:

- **Package** stages the player, named for the project, beside its
  `Paks/`. **Launch** runs the packaged game.
- **Project Settings** edits the project, including the packaged game's
  window (title, size, vsync) and the quality presets (Low, Medium, High,
  Epic).
- `aether_player --quality <preset>` picks a preset.

**Step 6: patches, DLCs and encryption** (§25.6).

- **Patches**: `aether_cook --patch-of Game.apak --pak-name Game_p1`
  writes only what changed since a release. The paths it removed are
  hidden in the paks below it.
- **DLCs**: `aether_cook --dlc Forest --dlc-base Game.apak --always
  DLC/Forest/` cooks extra content that the game merges in.
- **Encryption**: `--key` (ChaCha20, `aether_pak keygen` makes one)
  encrypts paks so archive tools can't read them. The player takes the
  key from `--key`, `AETHER_PAK_KEY`, or its build. The editor's Build
  and Package window has an Encrypt option.

**Phase 26 step 1: plugins** (§26.1). A plugin is a folder holding a
`<Name>.aplugin` descriptor, modules (code) and content:

- **Where they live**: the engine's in `plugins/`, a project's in its
  `Plugins/` folder.
- **Engine plugins**: Physics, Audio, Navigation, AI and Networking are
  plugins now, enabled by default.
- **Loading**: plugins are resolved with their dependencies and version
  checks. Their modules start in order, in the player and in the editor.
  The cooker records which ones the game runs and cooks their content.
- **Editor**: the **Plugins** panel enables a plugin for the project, and
  **New Plugin** makes one.

**Phase 26 step 2: project templates** (§26.2). New Project starts from a
template, each with a controller script, input bindings and a spinning
pickup Blueprint:

- **Blank**: a camera and a Player Start.
- **First Person**, **Third Person**, **Top Down** and **Vehicle**: the
  controller reads the bindings it ships, and is tested by running it with
  simulated keys and mouse.
- **2D Platformer**: a tilemap level, a character with coyote time, jump buffering and variable jump height, and a camera that follows it, all on the 2D toolkit and playable in the packaged player.
- **Editor**: the **New Project** tool creates one and opens it.
- **Playing**: the packaged player runs them, with scripts, Blueprints and
  input: `aether_player --press W --report` holds a key in a headless run
  and prints where the player ended up.

**2D** (§26.6, `sprite2d/`). Sprite atlases (`.aatlas`: frames, pivots and
animation clips, packed or cut from a sheet), `Sprite` and `SpriteAnimator`
components, tilesets (`.atileset`, with solid tiles and four-neighbour
autotiles), tilemaps (`.atilemap`), draw batches sorted by layer, order and
depth with view culling, and a pixel-perfect camera (whole-number scaling,
snapping). The **Tilemap** tool (under **2D**) paints, fills and erases on layers, sets solid tiles and edits autotile rules. 2D physics (`Rigidbody2D`, `Collider2D`: boxes and circles, triggers, layers, solid tilemap cells, ray and overlap queries, grounding for platformers) and 2D lights (global, point and spot lights, shadows from solid tiles and shadow-caster boxes, light maps and visibility polygons) are in.

**Sequencer** (§27.1, `sequencer/`). A level sequence (`.asequence`) animates entities over time: Transform tracks (position curves and slerped rotation keys) and Property tracks (any reflected float, integer, bool, enum or all-float struct field), with constant, linear and Bezier keys. A `SequencePlayer` plays, loops, scrubs and reverses it deterministically, and reports problems (a missing entity or field) once without stopping the other tracks. Event tracks fire named moments once as the playhead crosses them; Visibility tracks show and hide an entity; Spawn tracks keep a prefab alive for a range; Camera Cut tracks pick the camera the game renders from; Audio and Animation tracks start and stop cues and montages. The packaged player plays them: `.asequence` is a cooked asset type, and a `SequenceComponent` plays a sequence from an entity (the **Sequencer** tool, under **Cinematics**, edits them: a timeline with tracks, keys, scrubbing and undo), with Play/Pause/Stop/Set Time as Blueprint nodes and Luau calls, run by the `SequenceSystem`.

**Documentation** (§26.7). The manual is in [docs/manual](docs/manual/README.md).
The API reference is generated from the engine's reflection: run
`aether_docgen --out api` for `API.md` (every component, struct and enum with
its fields, ranges, units and tooltips) and `api.json`. The Windows release zip
includes both.

**Version control** (§26.7). The **Content Browser** tool (Project) lists the
project's folders and assets with a git badge on each: **M** modified, **A**
added, **?** untracked, **D** deleted, **R** renamed, **!** conflict (a folder
shows its worst child; an asset also counts its `.ameta`), with a "changed
only" filter. A project that isn't a git repository just says so.

**Extending the editor** (§26.5). Plugins, game modules and a project's
own Luau scripts add panels, menus, Inspector property drawers and asset
types. A script in `Content/Editor/*.luau`:

```lua
local clicks = 0
editor.AddPanel("Hello Panel", function()
    if ui.Button("Click me") then clicks += 1 end
    ui.SameLine()
    ui.Text("clicks: " .. clicks)
end)
editor.AddMenuItem("Tools/Hello/Say hello", function() editor.Log("hi") end, "Ctrl+Shift+H")
```

The editor's sample project ships this one: open the **Hello Panel** tool.

**Every tool in both editors.** The Windows editor (`aether_editor.exe`,
D3D12) and the portable shell (`aether_editor_shell`, Vulkan) host the
same set of tool editors (`editor/src/workspace`). Each one opens on a
sample document:

- **Scripting**: the Blueprint editor and the Luau code editor.
- **Rendering**: the Material editor and the particle system (VFX) editor.
- **Animation**: the anim graph, blend space and clip viewer.
- **AI**: the Behavior Tree editor and the Navigation panel (baked).
- **Audio**: the Sound Cue editor and the mixer.
- **UI**: the UI Designer.
- **World**: terrain, foliage, spline and world partition tools.
- **Networking**: networked Play-in-Editor and the network profiler.
- **Debug**: the console, the profiler and the crash reports.
- **Project**: Project Settings, and Build and Package.

They're in the **Tools** window (a list by category, with the selected
tool beside it) and the **Tools** menu, which pops a tool out into its
own window. `AETHER_EDITOR_TAB=<name>` picks the tool to open on.

## Building

Requires CMake 3.20+, a C++20 compiler with SSE4/AVX2 support (MSVC, Clang,
or GCC), and network access the first time you configure (to fetch Jolt
Physics, Dear ImGui, Vulkan-Headers, stb_image, nlohmann/json, and — on a
Vulkan-enabled Windows build — a SPIR-V-capable `dxcompiler.dll` release).

```bash
cmake -S . -B build
cmake --build build --config RelWithDebInfo
./build/sandbox/aether_sandbox.exe          # Windows only; Phase 3/4 bindless + GPU-culling demo
./build/editor/aether_editor.exe            # Windows only; the D3D12 editor: 3D view, physics and every tool editor
AETHER_RHI_BACKEND=vulkan ./build/rhi_demo/aether_rhi_demo.exe   # or =d3d12 (default); swappable RHI proof
ctest --test-dir build --output-on-failure
```

Set `-DAETHER_BUILD_PHYSICS=OFF`, `-DAETHER_BUILD_EDITOR=OFF`, and/or
`-DAETHER_BUILD_VULKAN=OFF` to skip the corresponding fetches (e.g. for a
quick engine-only build with no network).
