# Cluster-based geometry LOD: implementation sequence

Status: **in progress**. Initial CPU reference: `engine/include/aether/cook/cluster_reference.h`.

## Existing infrastructure

- `engine/include/aether/cook/meshlet_cook.h`: deterministic meshlets, bounds, simplified LODs and error values.
- `engine/include/aether/cook/mesh_cook.h`: cooked primitive with LOD meshlets; existing AMSC v1 serialized format.
- `AGENTS.md`: full CTest gate and measured performance requirements.

## Current CPU reference (first increment)

`SelectReferenceLod` selects the coarsest existing whole-primitive LOD under a
pixel error budget. `CullReferenceMeshlets` tests its meshlet bounding spheres
against frustum planes. Inputs are **model space**. The camera and frustum must
be transformed into model space before calling; non-uniform scale requires
special care when projecting geometric error.

This is deliberately a **whole-primitive LOD chain**, not a per-cluster DAG.
It cannot perform mixed-detail cluster cuts, eliminate T-junctions or provide
virtualized geometry. Do not label it Nanite parity.

## Next PR: cooker hierarchy

1. Group adjacent fine meshlets into connected spatial clusters. Generate parent
   proxy geometry using the existing meshoptimizer simplification and meshlet
   builders; record conservative parent geometric deviation, bounds, and child
   references. Do not infer parent error solely from triangle count.
2. Represent a versioned DAG with explicit root IDs, child ranges, geometry
   references and error. Enforce acyclicity, index bounds, deterministic ordering,
   coverage and a documented ownership rule for shared children.
3. Define a crack prevention policy at LOD boundaries before shipping mixed cuts.
   If the cooker cannot prove watertightness, retain whole-primitive LOD fallback.
4. Extend the cooked format with an explicit version bump and strict byte-count
   validation; keep v1 loading working and fuzz the new reader.

## CPU reference correctness tests (required before GPU work)

- Zero error, zero viewport, invalid FOV, camera inside bounds, tiny distances.
- LOD choice at threshold and near/far cameras; monotonic conservative error.
- Plane tangency and non-unit normals; empty frustum and rejected meshlets.
- Multi-root and shared-child DAGs: no duplicate draws, missing coverage or cycles.
- Skinned primitive fallback; malformed cooked data; transform scaling.
- Camera sweeps across LOD boundaries, with screenshot or coverage regression.

## Benchmark gate

Before claiming a speedup, record the existing draw path and CPU reference on
the same scene, build, hardware, resolution and camera path. Report median/p95/p99
CPU culling time, total CPU/GPU frame time, draw calls, meshlets and triangles
submitted, peak memory, cooker time and cooked asset size. Keep negative results.
Follow `AGENTS.md` and `docs/design/PERFORMANCE.md`.

## Validation

Required: configure, compile, and run the **entire** `ctest` suite on a suitable
checkout. A successful GitHub file commit does not constitute a passing build.
Only after the CPU reference and DAG are verified should GPU culling, indirect
draws, residency and streaming be separate follow-up PRs.
