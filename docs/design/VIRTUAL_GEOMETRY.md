# Virtual geometry (a Nanite-style renderer, in stages)

This is the same effort as `CLUSTER_GEOMETRY_LOD.md`, which holds the validation and benchmark gates the stages below must meet and the status
against its list. `cluster_reference.h` there is the whole-mesh LOD reference; the hierarchy here is the per-cluster one.

The goal is meshes of any density with automatic detail: the renderer draws only as many triangles as the screen can show, with no popping and no cracks.
This is built in stages, each tested before the next starts. This document records the plan, what stage 1 does and what it measured.

## The plan

1. **Cluster hierarchy and a CPU reference selector** (this stage, `engine/include/aether/cook/cluster_hierarchy.h`). Offline, CPU only, no cooked format
   change.
2. **GPU culling.** A compute pass runs the same test as `SelectClusters` on every cluster, plus frustum, normal-cone and occlusion (Hi-Z) culling, and writes an
   indirect draw list. The CPU selector is its reference.
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

### Known limits

- **It stops at 54 root clusters (1.6% of the triangles), not at one.** Locked group borders keep the top levels from shrinking further, and a group that
  cannot shrink waits to be merged at a later level. A later stage can regroup across levels or lift the locks near the root.
- The simplifier works on positions only: UV seams and normals are not protected, so a textured mesh may show seams at coarse levels until attribute-aware
  simplification is added.
- Everything is in memory: nothing is cooked or drawn yet. Stage 3 changes the cooked format and so the format-versions table.
- Whether clusters beat the current draw path is **unmeasured**: that needs the GPU stages and a real GPU. It is the first thing to measure in stage 2.
