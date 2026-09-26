# Aether

A data-oriented, job-based game engine targeting Vulkan/DirectX 12. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) *(coming later)* for the full
design.

The plan for growing Aether into a full engine (editor, visual scripting /
Blueprints, text scripting, animation, audio, runtime UI, packaging, ...) is in
[`docs/ROADMAP.md`](docs/ROADMAP.md); all planning and design docs are listed in
[`docs/README.md`](docs/README.md).

## Status: Phase 8 — Asset System (in progress; Phase 7 editor UI items pending a Windows build)

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

### Phase 8 (in progress) — Asset System

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
