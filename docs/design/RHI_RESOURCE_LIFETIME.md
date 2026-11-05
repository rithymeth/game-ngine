# RHI resource lifetime

How GPU objects made through `IDevice` are owned, destroyed and retired. Applies to the Vulkan and D3D12 backends alike.

## Ownership

Everything `IDevice::Create*` returns is owned by the caller until it is destroyed; the device only owns what is still alive when it is
torn down.

| Created by | Destroyed by |
| --- | --- |
| `CreateVertexBuffer`, `CreateIndexBuffer`, `CreateConstantBuffer`, `CreateStorageBuffer`, `CreateFrameConstantBuffer` | `DestroyBuffer` |
| `CreatePipeline` | `DestroyPipeline` |
| `CreateComputePipeline` | `DestroyComputePipeline` |
| `CreateTexture`, `CreateR32FloatTexture`, `CreateCompressedTexture` | `DestroyTexture` |

Each `Destroy*` frees the native objects (buffer memory and mapping, a frame-constant buffer's descriptor set or heap slot, a storage
buffer's UAV slot, pipeline layouts and descriptor-set layouts, an image and its view) and returns the slot to a free list that the next
`Create*` of that kind reuses first. The defaults on `IDevice` do nothing and return `false`.

Owners in the engine: `ParticlePass`, `DecalPass`, `TerrainPass`, `RhiTerrainGpu`, `GpuEmitterRuntime` and `SceneRenderer` release what they
create (in their destructors, and `ParticlePass` also when it outgrows a vertex buffer). Their destructors are safe right after the last
`Draw`: the device keeps the objects alive until the frames that read them finish.

## Handle validity

Handles are plain table indices; they carry no generation. The rules:

* A handle is valid from `Create*` until the first `Destroy*` of it. `Destroy*` returns `false` and changes nothing (one logged error)
  for an invalid handle, an out-of-range one, or one already destroyed.
* Updates (`UpdateVertexBuffer`, `UpdateConstantBuffer`, ...) and command-list binds (`BindPipeline`, `BindVertexBuffer`, `BindComputeStorageBuffer`,
  `BindFrameConstants`, ...) with a destroyed handle are refused with a logged error; nothing is recorded.
* Once a slot is reused, an old copy of the handle names the new resource. This cannot be detected without a generation counter, which
  would change `BufferHandle` / `PipelineHandle` / `ComputePipelineHandle` and every place that builds one; it was left out. Drop your copy
  when you destroy (`handle = {}`) and keep one owner per resource.
* Each table is separate: a `BufferHandle` index is never a `PipelineHandle`.

## Deferred destruction (retire queue)

Destroying never calls `vkDeviceWaitIdle` or idles a D3D12 queue. Instead:

1. The slot is cleared and put on the free list at once (so the handle is dead and the slot reusable).
2. The native objects move into a closure on the device's retire queue, tagged with the graphics and compute fence values that were the
   latest *submitted* at that moment (Vulkan: the timeline semaphores' last signalled values; D3D12: the queue fences).
3. A queue entry runs when both fences have completed (`IsFenceComplete`, `IsComputeFenceComplete`), so every submission that could have
   used the resource has finished. With an idle GPU that is immediately, inside the `Destroy*` call.
4. The queue is drained from `Submit`, `SubmitCompute`, `WaitForFence`, `WaitForComputeFence`, every `Destroy*`, and the resource-creating
   calls that depend on freed capacity (frame-constant buffers, bindless textures). `ReleaseRetiredResources()` drains it on demand
   and never blocks. `GetResourceStats().pending_retire` is its length.

A command list that was *recorded but not yet submitted* with a resource is the caller's bug: the fence bound only covers submissions made
before the destroy.

### Bindless textures

The bindless table has no update-after-bind, so a descriptor in use by a submitted command buffer cannot be rewritten. `DestroyTexture`
therefore does everything at retirement: it rewrites the slot to the white fallback, releases the image, and only then makes the slot
reusable. Until retirement a stale handle still samples the old image (kept alive, so it reads valid memory); afterwards it samples white.
The slot index is never reused before retirement, so a new texture is never written under a submission still reading the old one.
`RHI_*DestroyTextureFreesTheSlotForReuse` holds because the test GPU is idle, so retirement is immediate.

## Teardown and leak report

Device destruction waits for the queues to idle, then runs the whole retire queue, then frees whatever is still in the tables. Records
the free lists emptied are skipped, so each native object is freed exactly once. If buffers, pipelines, compute pipelines or textures are
still live, one warning with the counts is logged (`Device torn down with live resources: ...`): cheap counters, no per-object tracking.

## Counters

`IDevice::GetResourceStats()` reports live buffers, pipelines, compute pipelines and textures, the table slot counts (bounded by reuse) and
the retire-queue length. Tests assert these stay flat across thousands of create/destroy rounds
(`tests/test_rhi_lifetime.cpp`, on both backends).

## Not covered

* Texture and buffer *contents* are not synchronised by destruction; `UpdateVertexBuffer` and friends still idle the queue they write under.
* Handles from `ReserveExternalBindlessSlot` belong to their swap chain and are refused by `DestroyTexture`.
