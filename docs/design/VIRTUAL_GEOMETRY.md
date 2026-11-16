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
3. **Indirect rendering.** One multi-draw of the surviving clusters into the existing depth and G-buffer passes and the existing materials. This is where the
   cooked mesh format gains the hierarchy and the plugin shell appears.
4. **Streaming** of cluster pages by what is visible.
5. Software rasterization of triangles smaller than a pixel is **not planned**: it is the hardest part and the other stages deliver most of the benefit.

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

### Known limits

- **Fixed 16 November 2026: compute results did not reach the CPU on a real GPU.** The Vulkan backend bound each compute descriptor set before writing the buffers into it; lavapipe tolerated that, NVIDIA read empty descriptors, so every compute shader wrote nothing. Sets are now bound in `Dispatch`. `ComputeReadback_AShaderWriteReachesTheCpu_Vulkan` guards it and `ClusterCullGpu_*` now agree with the CPU on the RTX 3050.
- **It stops at 54 root clusters (1.6% of the triangles), not at one.** Locked group borders keep the top levels from shrinking further, and a group that
  cannot shrink waits to be merged at a later level. A later stage can regroup across levels or lift the locks near the root.
- The simplifier works on positions only: UV seams and normals are not protected, so a textured mesh may show seams at coarse levels until attribute-aware
  simplification is added.
- Stage 3 draws clusters on Vulkan (below) but is not wired into the player or the scene renderer, has no Hi-Z occlusion, no streaming, and **its speed against the existing draw path is unmeasured** (the benchmark gate in `CLUSTER_GEOMETRY_LOD.md`). D3D12 has no indirect draw yet.
- No occlusion (Hi-Z) culling yet: it needs a depth pyramid, which is a graphics-side pass.
- Whether clusters beat the current draw path is **still unmeasured**. This container has only a software Vulkan device (lavapipe), so the GPU pass is verified
  for correctness, not speed. Measuring it on a real GPU against the existing draw path is the first thing to do before stage 3, and the design doc's benchmark gate
  (`CLUSTER_GEOMETRY_LOD.md`) applies.
