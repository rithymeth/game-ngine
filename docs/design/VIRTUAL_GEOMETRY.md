# Virtual geometry (a Nanite-style renderer, in stages)

This is the same effort as `CLUSTER_GEOMETRY_LOD.md`, which holds the validation and benchmark gates the stages below must meet and the status
against its list. `cluster_reference.h` there is the whole-mesh LOD reference; the hierarchy here is the per-cluster one.

The goal is meshes of any density with automatic detail: the renderer draws only as many triangles as the screen can show, with no popping and no cracks.
This is built in stages, each tested before the next starts. This document records the plan, what stage 1 does and what it measured.

## The plan

1. **Cluster hierarchy and a CPU reference selector** (this stage, `engine/include/aether/cook/cluster_hierarchy.h`). Offline, CPU only, no cooked format
   change.
2. **GPU culling** (done, below). A compute pass runs the same test as `SelectClusters` on every cluster, plus frustum and normal-cone culling, and writes a list
   of the clusters to draw. The CPU selector is its reference. Occlusion (Hi-Z) culling and the indirect draw list are not in this stage.
3. **Indirect rendering** (done, below). One multi-draw of the surviving clusters; the cooked mesh format gains the hierarchy.
4. **The player draws through clusters** (done, opt-in, below), measured on a real scene.
5. **Streaming** of cluster pages by what is visible.
6. Software rasterization of triangles smaller than a pixel is **not planned**: it is the hardest part and the other stages deliver most of the benefit.

## Stage 1: how the hierarchy works

The mesh is cut into clusters of at most 124 triangles and 64 vertices (the existing meshlet builder), the **leaves**: the full-detail mesh, exactly. Each level
groups neighbouring clusters (four by default, grown by shared vertices from seeds in Morton order), merges a group's triangles, simplifies the merge to about
half with its **border locked**, and cuts the result into new clusters. It repeats until nothing more can be simplified. The result is a DAG: a cluster's parent
is the group that merged it, and a group has several outputs.

Each cluster carries an object-space `error` (0 for a leaf) and a bounding sphere, both shared by every cluster its source group made, and the same two things
for the group that merges it (`parent_error`, `parent_*`). A group's error is above its inputs' (the largest input error plus what its own simplification cost)
and its sphere holds theirs. That is the whole trick: **a cluster is drawn when its own error looks small enough on screen but its parent's does not.**
`ProjectedError` turns an error into pixels at a distance, and `SelectClusters` applies the test. It looks at one cluster alone, so it parallelises
trivially, and because group borders are locked and a group's clusters all decide together, the clusters picked always join with no cracks.

### What the tests prove

- The leaves are the source mesh exactly, and every cluster of every level is within the meshlet limits.
- Every group's outputs have at most 85% of its inputs' triangles; errors and spheres only grow towards the root.
- **The cut is watertight at every distance**: the selected triangles have no boundary edge the source mesh does not have (no cracks, no holes) and no edge
  shared by more than two triangles (no overlap). Dropping the border lock, the error accumulation or the parent test each fails this test.
- At a threshold of 0 the cut is the leaves, the mesh exactly; further away it never has more triangles; frustum culling drops what is out of view.

### Measured

A lumpy sphere of 102 400 triangles (software-only, one thread, a Linux container: relative numbers, not a GPU benchmark):

| | |
| --- | --- |
| Build | 2.5 s for 1 092 leaves, 3 067 clusters in 827 groups over 12 levels |
| Selection at 1 px, camera 1.5 m away | 717 clusters, 44 098 triangles |
| 8 m | 359 clusters, 17 146 triangles |
| 80 m | 170 clusters, 6 860 triangles |
| 2 000 m | 54 clusters, 1 598 triangles |
| Selection cost | about 50 microseconds on the CPU for 3 067 clusters, whatever the distance |

## Stage 2: the cut and the culling on the GPU

`player/src/cluster_cull_pass.h` is a compute pass: one thread per cluster runs the same test as `cook::SelectClusters` (its own error projects to at most the pixel
threshold, its parent's to more), then the frustum test and the normal-cone back-face test, and appends the survivors to a list with an atomic counter (the list's
order is therefore not fixed). `cook::SelectClusters` gained `cull_backfaces` so the CPU reference has the cone test too.

To check it on real hardware the RHI gained two optional calls, `ICommandList::HostReadBarrier` and `IDevice::ReadStorageBuffer` (with
`SupportsStorageBufferRead`, false by default). Vulkan implements them; **D3D12 does not yet, so the GPU test skips there with a note.** They read the storage buffer's
own host-visible memory, which is slow on a real GPU; a shipping pass will copy to a readback buffer or, as stage 3 will, read the list on the GPU.

### What the tests prove (on Vulkan: lavapipe in this container)

- The GPU picks exactly the same clusters as the CPU selector for 7 distances x 4 thresholds x 4 culling modes (none, frustum, cone, both): 112 runs, zero
  differences, and the same triangle totals.
- Culling and the threshold move the list the right way; a frustum that sees nothing gives an empty list; the pass can be reused (its counters start at zero).
- A full output reports how many clusters were needed, and what fit is a subset of the right answer.
- A cone-culled cluster has every triangle facing away from the viewer (checked on the real triangles).
- Dropping the parent test, flipping the frustum comparison, inverting the cone test or not counting triangles each fails the comparison.

### Benchmark: cluster culling on the CPU against the GPU

`bench/src/bench_cluster_cull.cpp` (in `aether_bench`) times one cut-and-cull of a lumpy sphere's whole hierarchy: on the CPU (`cluster/cpu_select_*`), and on the
GPU as one cull with its submit and wait (`cluster/gpu_cull_*`, the latency of asking once) and as 16 dispatches in one submission (`cluster/gpu_cull_x16_*`, divide by
16). It answers one narrow question, whether culling clusters on the GPU is cheaper than on the CPU at a given scale. It is **not** the benchmark gate in
`CLUSTER_GEOMETRY_LOD.md` (the existing draw path against cluster rendering): that needs the draw stage.

To run it on a Windows machine with a real GPU (the GPU entries use Vulkan, which NVIDIA, AMD and Intel drivers provide):

```
cmake --build build --config RelWithDebInfo --target aether_bench
build\bench\RelWithDebInfo\aether_bench.exe --filter cluster/ --repeat 15
```

Set `AETHER_BENCH_CLUSTER_LARGE=1` first to add a 1-million-triangle density (its hierarchy takes tens of seconds to build). Without a Vulkan device the GPU entries
do nothing and say so on stderr. The same machine also runs the GPU-against-CPU comparison test for real: `AETHER_TEST_FILTER=ClusterCullGpu_` with `aether_tests`.

Measured in the Linux container (the only numbers taken so far; **the GPU rows ran on lavapipe, a CPU emulation of Vulkan, and say nothing about a real GPU**):

| | clusters | median |
| --- | --- | --- |
| CPU `SelectClusters`, 68k-triangle sphere | 2 010 | 0.033 ms |
| CPU `SelectClusters`, 212k-triangle sphere | 6 389 | 0.104 ms |

The CPU selector costs about 16 ns per cluster on one thread, so it is cheap until there are many tens of thousands of clusters in view (many instances of dense
meshes): that is where a GPU pass has to win, and where the real-GPU numbers are needed.

Measured on a real GPU: **NVIDIA GeForce RTX 3050 Laptop (Vulkan, discrete). This is not an RTX 3060**; GDD section 57's 3060 class is still unmeasured.
`aether_bench --filter cluster/ --repeat 15`, RelWithDebInfo, 16 November 2026 (calibration 1.76 ms), **after** the descriptor-binding fix below. An earlier run the same day
is withdrawn: its GPU cull read empty descriptors and so measured an empty shader.

| | clusters | median | p95 | per cull |
| --- | --- | --- | --- | --- |
| CPU `SelectClusters`, 68k triangles | 2 050 | 0.062 ms | 0.065 ms | 0.062 ms |
| GPU one cull + submit + wait, 68k | 2 050 | 0.145 ms | 0.202 ms | 0.145 ms |
| GPU 16 culls in one submission, 68k | 2 050 | 0.224 ms | 0.301 ms | 0.014 ms |
| CPU `SelectClusters`, 212k triangles | 6 349 | 0.098 ms | 0.099 ms | 0.098 ms |
| GPU one cull + submit + wait, 212k | 6 349 | 0.693 ms | 1.296 ms | 0.693 ms |
| GPU 16 culls in one submission, 212k | 6 349 | 1.043 ms | 1.555 ms | 0.065 ms |

Reading: a single GPU cull is **slower** than the CPU at both sizes (2.3x at 68k, 7x at 212k), so culling one view on the GPU does not pay for itself at these
cluster counts. Batched, the GPU is 4.4x cheaper at 68k and only 1.5x cheaper at 212k, and the 212k batch is dominated by something other than launch cost
(it scales 4.6x for 3.1x the clusters), which is worth profiling before stage 3 relies on it. The case for the GPU pass is that its list stays on the GPU for an
indirect draw (no readback, no CPU cull), not that the cull itself is faster. This measures culling only, not the draw-path comparison the gate asks for.

## Stage 3: the cooked cluster hierarchy (AMSC v3)

`MeshCookSettings::build_clusters` (off by default; skinned primitives are skipped) bakes each primitive's `ClusterHierarchy` from its level-0 mesh. `SaveAmesh` writes
version 3: after the HLOD table, a count and then, for each primitive that has one, its index and the hierarchy (indices, 92-byte clusters, groups). Version 1 and 2 files
still load, with no clusters. The reader checks every count against the bytes left and every reference against what it points into (indices against the primitive's
vertices, a cluster's triangles against the index array, group references and members against their tables, finite and non-negative bounds), and
`MeshCook_RefusesClusterHierarchiesThatPointOutsideThemselves` tampers with each of those. A loaded hierarchy selects exactly the clusters the cooked one does. The fuzz
corpus has a cluster seed (`tools/fuzz/corpus/amesh/valid_clusters.amesh`, about 28 KB for a 12 x 12 grid). The format-versions table row now says 3.

### The draw: the cull list drives the GPU draw

`ICommandList::DrawIndexedIndirect` reads one draw record from a storage buffer; `DrawIndexedIndirectCount` reads up to `max_draws` records and the number to draw from a second
buffer (`IDevice::SupportsIndirectCount`: Vulkan 1.2 `drawIndirectCount` plus `multiDrawIndirect`, which the device now enables where it has them). The cull pass writes, for each
selected cluster, a record `{triangles * 3, 1, first index, 0, 0}` into `ClusterCullPass::DrawArguments()` next to the cluster list, and its atomic counter at offset 0 of
`DrawCount()` is the draw count (the draw clamps it to the capacity). Bind a vertex buffer of the mesh's vertices and an index buffer holding exactly `hierarchy.indices`, and nothing
is read back: the CPU never learns how many clusters were drawn. Compute and graphics are on different queues, so the graphics submission must wait on the compute fence
(`GraphicsQueueWaitOnCompute`); the test waits on the CPU instead.

`ClusterDrawGpu_TheCullListDrivesTheDrawWithNothingReadBack` renders a lumpy sphere (RTX 3050 Laptop, Vulkan): at a threshold of 0 the leaf cut is the whole mesh and the picture equals drawing
the mesh's own indices **pixel for pixel**; a coarser cut draws fewer than half the triangles and covers about the same silhouette; a frustum that sees nothing draws nothing; and the
call refuses a zero draw count, a non-storage buffer, more draws than the arguments hold, a misaligned offset and a dead handle.

### Benchmark: the draw path, one frame end to end

`bench/src/bench_cluster_draw.cpp` (`aether_bench --filter draw/ --repeat 15`) draws one lumpy sphere at 1920x1080 from 8 m three ways and times a whole frame (record, submit, wait for the GPU):
**direct** (the existing path: the whole mesh in one `DrawIndexed`, no LOD), **cpu_cull** (`SelectClusters` on the CPU, its draw records written by the CPU, one `DrawIndexedIndirectCount`) and
**gpu_cull** (the cull on the compute queue, the graphics queue waits on its fence, one `DrawIndexedIndirectCount`, no CPU work). The cluster paths use a 1-pixel error budget and back-face
culling. Measured on the **NVIDIA GeForce RTX 3050 Laptop (not a 3060)**, Vulkan, RelWithDebInfo, 17 November 2026, calibration 1.67 ms:

| scene | path | draws | triangles submitted | graphics GPU time | frame, median (p95) |
| --- | --- | --- | --- | --- | --- |
| 68k triangles | direct | 1 | 67 600 | 0.175 ms | 0.273 ms (0.310) |
| | cpu_cull | 261 | 14 061 | 0.100 ms | 0.251 ms (0.288) |
| | gpu_cull | 261 | 14 061 | 0.108 ms | 0.334 ms (0.383) |
| 212k triangles | direct | 1 | 211 600 | 1.943 ms | 3.199 ms (5.980) |
| | cpu_cull | 511 | 24 864 | 0.355 ms | 0.935 ms (3.694) |
| | gpu_cull | 511 | 24 864 | 0.306 ms | 0.661 ms (1.819) |

Reading, honestly:

- **At 68k the GPU path loses on frame time** (0.334 ms against 0.273 ms direct): the GPU spends less on the draw itself (0.108 against 0.175 ms) but the compute submission, the cross-queue wait and the
  cull cost about 0.2 ms more than that saves. Direct was already cheap.
- **At 212k it wins**: 0.661 ms against 3.199 ms direct (4.8x) and against 0.935 ms for the CPU cull (1.4x). Culling cut the triangles drawn from 211 600 to 24 864; the GPU time of the draw fell 6.3x.
  The CPU path's p95 (3.7 ms) shows its jitter. The 212k cull cost is also the one the earlier cull benchmark found disproportionate (see above), so there may be more to gain.
- **What this does not show.** One mesh, one instance, one camera; the sphere's triangles are sub-pixel on screen, which is the case cluster LOD is for, and a scene of larger triangles would favour
  `direct` more. `direct` also draws the back-facing half (nothing in this pipeline culls it), which the cluster paths drop, so part of the win is back-face culling, not LOD. No instancing, no
  occlusion, no shading cost, a laptop GPU, one run of 15 samples. Treat it as evidence that the machinery works and pays off on dense geometry, not as a general speedup claim; the same run on a 3060
  and on a real scene is still needed.

## Stage 4: the player draws through clusters (opt-in)

The scene renderer can draw a still, opaque, large primitive as the clusters that look right from where the camera is. It is **off unless asked for**:

| Environment variable | Meaning |
| --- | --- |
| `AETHER_VIRTUAL_GEOMETRY=1` | build a cluster hierarchy for each qualifying primitive when its model loads, and draw those primitives through it |
| `AETHER_VIRTUAL_GEOMETRY_MIN_TRIANGLES` | the smallest primitive that gets one (default 20,000) |
| `AETHER_VIRTUAL_GEOMETRY_PIXELS` | the error, in pixels, the cut may leave (default 1; 0 draws the finest clusters, which is the mesh) |
| `AETHER_PLAYER_VIRTUAL_GEOMETRY_STATS=1` | `aether_player` logs the triangles chosen against the triangles of the same draws' meshes, twice a second |

Each frame, for every such draw the camera sees, the GPU cull of stage 2 runs in the primitive's own space (the camera and the frustum are brought through the
inverse model matrix) on the compute queue; one indirect draw (stage 3's `DrawIndexedIndirectCount`) in the main colour pass then draws what it chose with the
primitive's own vertex buffer and the hierarchy's index buffer. The graphics submission waits for the compute one on the GPU. Shadows, the water's mirror, TAA's
velocity pass and ray-traced shadows keep drawing the whole mesh.

A primitive is left to the plain draw when it is skinned, wind-swayed, water, not opaque, drawn with a material graph, on a device without indirect-count draws or
storage-buffer reads, or when this draw's model matrix scales its axes unequally (more than 1% apart) or mirrors. The first two limits are because the cull needs one
pixel-error number per draw and the cones one winding. A model drawn by several entities gets a cull for each (up to 8 a frame); a slot's passes are per frame in
flight, so a cull never overwrites what an earlier frame's draw is still reading.

### Welding, and why

glTF models are very often stored with a vertex for every corner of every triangle (the test sphere has 245,760 vertices for 40,962 positions). The hierarchy builder
finds neighbours by shared vertex numbers, so such a mesh is a pile of unconnected triangles to it: nothing merges, there is one level, and building it took 19 s for
82,000 triangles. The renderer therefore welds vertices that have the same position, normal and UV before building (the 82k sphere then builds in 1.2 s into 15
levels), builds on the welded vertices **alone**, and maps the hierarchy's indices back into the draw's own vertex numbering. Two details cost time to find:

- Leaving the unused duplicates in the position array makes meshoptimizer treat them as seam partners and lock the mesh: the simplifier then removes nothing.
- Tangents differ between the copies (they are made a triangle at a time), so they cannot be part of the weld; the first copy's tangent is used. That only matters to
  a normal-mapped surface, and only by the difference between neighbouring triangles' tangents. A hard edge or a UV seam has different normals or UVs and stays split.

### What the tests prove (Vulkan, lavapipe)

`VirtualGeometryPlayer_*` render an 82k-triangle sphere through the whole renderer: off by default; at 0 pixels the picture is **pixel for pixel the plain one**; at 1
pixel the draw sends under a quarter of the triangles and the picture differs by under 0.5 of 255 on average; a model a quarter the size is cut harder; a model with
an unequal scale, or behind the camera, is not drawn through clusters (the first is drawn as before, byte for byte); clusters outside the picture are left out. Controls:
reading the whole mesh's indices, a camera that is not brought into the model's space, and a frustum that is not used each fail a test.

### Measured on the Woodland scene (lavapipe, a software renderer: not evidence about a GPU)

The baked Woodland has no mesh near 20,000 triangles: its ground is cut into 4,608-triangle tiles, and its 67k-triangle water is excluded. With the minimum lowered to
4,000, 74 primitives (ground tiles and the larger trees) get clusters in 1.3 s in all at load, and the aerial opening shot at 1280x720 draws 102 of them. At 1 pixel they
send **164,818 of 501,220 triangles (33%)**; at 0 pixels 465,494 (back faces and what is outside the view). Against the plain draw the picture differs in 0.01% of its
pixels at 0 pixels (nothing visible), and at 1 pixel in 42% of them by 0.66 of 255 on average, 0.5% by more than 8: coarser clusters shade a little differently.
Two plain runs are identical, so none of that is noise. The renderer's scene time on lavapipe was 714 ms against 767 ms, which says only that fewer triangles cost a
CPU rasterizer less. **Whether it is faster on a GPU is still unmeasured**; the stage 3 benchmark found the GPU path slower at 68k triangles and faster at 212k, and this
scene's primitives are all small. It will pay off on heavy meshes (scans, sculpts, generated models), not on content already cut into small tiles.

### Known limits of stage 4

- The hierarchy is built when the model loads, on the CPU (17 ms on average here, 1.2 s for 82k triangles). The cooked one (AMSC v3, opt-in at cook time) is not read by
  the player yet: the pak carries the cooked mesh, the player loads the glTF.
- One cull and one indirect draw per draw of a primitive: for many instances of one mesh that is the wrong shape (instancing is the answer there), which is why it
  is opt-in and bounded to 8 draws a primitive a frame.
- No Hi-Z occlusion, no streaming, no D3D12 (no indirect-count draw there yet), no skinned meshes.
- It was run on lavapipe only. The synchronisation (a cull per frame slot, the graphics queue waiting on the compute fence) has not been checked on a real GPU.

### Known limits

- **Fixed 16 November 2026: compute results did not reach the CPU on a real GPU.** The Vulkan backend bound each compute descriptor set before writing the buffers into it; lavapipe tolerated that, NVIDIA read empty descriptors, so every compute shader wrote nothing. Sets are now bound in `Dispatch`. `ComputeReadback_AShaderWriteReachesTheCpu_Vulkan` guards it and `ClusterCullGpu_*` now agree with the CPU on the RTX 3050.
- **It stops at 54 root clusters (1.6% of the triangles), not at one.** Locked group borders keep the top levels from shrinking further, and a group that
  cannot shrink waits to be merged at a later level. A later stage can regroup across levels or lift the locks near the root.
- The simplifier works on positions only: UV seams and normals are not protected, so a textured mesh may show seams at coarse levels until attribute-aware
  simplification is added.
- Stage 3 draws clusters on Vulkan (below); stage 4 wires it into the player behind an opt-in switch. It has no Hi-Z occlusion, no streaming, and **its speed against the existing draw path is unmeasured** (the benchmark gate in `CLUSTER_GEOMETRY_LOD.md`). D3D12 has no indirect draw yet.
- No occlusion (Hi-Z) culling yet: it needs a depth pyramid, which is a graphics-side pass.
- Whether clusters beat the current draw path is **still unmeasured**. This container has only a software Vulkan device (lavapipe), so the GPU pass is verified
  for correctness, not speed. Measuring it on a real GPU against the existing draw path is the first thing to do before stage 3, and the design doc's benchmark gate
  (`CLUSTER_GEOMETRY_LOD.md`) applies.
