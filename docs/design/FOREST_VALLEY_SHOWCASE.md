# Forest Valley: the Aether integrated showcase

Status: **proposal.** Every number below is a target, not a measurement of the current engine. Nothing here has been
built; the first step is to measure the existing renderer (Phase 4, pulled forward to Phase 1).

Forest Valley is a 512 x 512 m explorable 3D environment with a controllable character. It exists to validate the
engine as an integrated whole (rendering, physics, destruction, audio, editor, packaging), not to be a game. It adds no
quests, inventory, combat or multiplayer.

## Specification

| Item | Target |
| --- | --- |
| World size | 512 x 512 m, five areas, walkable in 5-10 minutes |
| Performance | 1080p, 60 FPS on a named midrange GPU |
| Primary platform | Windows / D3D12 |
| Secondary validation | Linux / Vulkan |

## Areas

| Area | Content | Tests |
| --- | --- | --- |
| A. Starting Meadow | Spawn, grass, flowers, cabin, ambient audio, valley view | Vegetation, character movement, lighting, shadows |
| B. Ancient Woodland | Dense trees, ferns, moss, rocks, logs | Instancing, culling, LOD, streaming, frame-time stability |
| C. River and Waterfall | Water, shoreline, bridge, waterfall, mist | Reflections, transparency, particles, water shader, audio |
| D. Ancient Ruins | Stone architecture, doors, breakables | PBR, collision, physics, destruction |
| E. Mountain Overlook | Elevated viewpoint over the valley | Long-distance visibility, LOD, atmosphere, streaming |

## Rendering priorities

| System | Requirement | Priority |
| --- | --- | --- |
| PBR materials | Rock, wood, soil, vegetation, metal | P0 |
| Directional light | Sun and cascaded shadows | P0 |
| Atmosphere | Fog, sky, distance haze | P0 |
| HDR and tone mapping | Consistent indoor/outdoor exposure | P0 |
| Vegetation | Instanced trees, grass, foliage | P0 |
| Water | River surface and shoreline | P1 |
| Reflections | SSR with a defined fallback | P1 |
| Post-processing | Bloom, DOF, auto exposure | P1 |
| Hierarchical geometry | Cooker DAG, CPU reference validation | P1 |
| GPU cluster culling, geometry streaming | Later milestone | P2 |

P0 = first playable. P1 = polished. P2 = later rendering milestone.
**The first version reuses the existing renderer. The showcase must not depend on the unfinished cluster DAG.**

## Asset budget (planning quantities)

| Category | Instances |
| --- | --- |
| Trees | 800-1,500 |
| Grass and plants | 12,000-30,000 |
| Rocks | 500-1,000 |
| Logs and branches | 150-350 |
| Architecture and props | 100-250 |
| Interactive physics objects | 20-60 |

About 30-60 reusable source meshes, heavily instanced.

## Interactions and debug menu

Walk, run, jump, look, cross the bridge, open a door, push a physics object, trigger one destruction event. A debug
menu changes time of day, toggles effects, shows visible-object counts and captures performance measurements.

## Acceptance gates (proposed)

| Metric | Gate |
| --- | --- |
| CPU frame p95 | <= 16.67 ms |
| GPU frame p95 | <= 14 ms |
| Frame p99 | <= 20 ms |
| Soak | 30 minutes |
| Camera benchmark | Fixed, repeatable route |

Compare optimizations only on the same hardware, route and settings. Record results in `PERFORMANCE.md`, including
negative ones.

## Roadmap

1. **Foundation:** standalone showcase project (siblings: `samples/`, `games/`); terrain and reusable meshes; player
   camera and movement; Windows build and packaged launch. **Also capture the baseline renderer measurement here.**
2. **Environment:** populate five areas; terrain materials, vegetation, lighting; river, waterfall, fog; the camera route.
3. **Interactions:** Jolt collisions and props; destruction with safe debris limits; ambient audio and particles; debug
   graphics controls.
4. **Optimization:** profile CPU/GPU and streaming; integrate validated cluster LOD when ready; quality transitions and memory.
5. **Release verification:** full CTest; Windows/MSVC/D3D12 packaged build; Linux/Vulkan validation; screenshots,
   performance report, limitations.

## Deliverables

The 512 x 512 m environment, a packaged Windows build, a repeatable benchmark, consistent-settings screenshots, a
CPU/GPU profiling report, automated scene and rendering regression tests, and documentation of verified capabilities,
limitations and remaining work.

## Existing modules to build on

`terrain/`, `timeofday/`, `vfx/`, `streaming/`, `physics/`, `destruction/`, `audio/`, `player/`, `sequencer/` (camera
route). Check each against the showcase needs before assuming coverage; gaps found become the work list.

## Open questions

- Which GPU is the "midrange" reference?
- Does the cluster/hierarchy work in draft PR #989 land before Phase 4, or does the showcase run on the old path first?
- Where do source meshes come from (authored, glTF import, procedural), and under what license?
