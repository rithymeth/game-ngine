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
plus one 2000-frame run. GPU vertex skinning itself (actually consuming
`joint_indices`/`joint_weights`/`ComputeSkinMatrices` in a skinned draw
call) is not wired into a demo yet — the parsed data and the CPU-side
skin-matrix math are there and unit-tested, but no shader reads them yet.

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

## Building

Requires CMake 3.20+, a C++20 compiler with SSE4/AVX2 support (MSVC, Clang,
or GCC), and network access the first time you configure (to fetch Jolt
Physics, Dear ImGui, Vulkan-Headers, stb_image, nlohmann/json, and — on a
Vulkan-enabled Windows build — a SPIR-V-capable `dxcompiler.dll` release).

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
