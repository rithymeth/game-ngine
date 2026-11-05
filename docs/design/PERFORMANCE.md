# Performance record

Numbers from CI, not from a developer machine. Each table says where it came from, so the next one can be compared with it.

## Benchmarks: Linux GCC, RelWithDebInfo, GitHub-hosted runner

Source: the `Linux (GCC)` job of the run for PR #212 (commit `88e4bdf`, 2026-10-13), `aether_bench --repeat 5`. The
runner's calibration loop took 1.722 ms; `norm` is the median divided by it, which is what the baseline comparison
uses so a slow or noisy runner does not fail the gate.

| benchmark | median ms | p95 ms | min ms | norm |
| --- | ---: | ---: | ---: | ---: |
| ecs/iterate_100k | 0.0970 | 0.1407 | 0.0925 | 0.056 |
| ecs/query_50k_of_100k | 0.0012 | 0.0012 | 0.0012 | 0.001 |
| ecs/create_destroy_10k | 0.6301 | 0.6961 | 0.6273 | 0.366 |
| ecs/add_remove_component_10k | 1.5842 | 1.5996 | 1.5740 | 0.921 |
| jobs/batch_1024 | 0.1157 | 0.1250 | 0.1146 | 0.067 |
| scene/save_binary_10k | 26.5757 | 29.7012 | 26.1082 | 15.447 |
| scene/load_binary_10k | 35.6088 | 36.0391 | 35.2799 | 20.698 |
| scene/save_json_10k | 39.3688 | 40.8060 | 39.3143 | 22.883 |
| scene/load_json_10k | 57.8340 | 68.7669 | 57.1742 | 33.616 |
| cook/texture_bc7_256 | 226.4041 | 226.7629 | 226.0741 | 131.598 |
| cook/texture_bc1_512_mips | 140.3769 | 140.8718 | 140.2251 | 81.595 |
| cook/atex_roundtrip_256 | 0.0035 | 0.0036 | 0.0035 | 0.002 |
| world/flythrough_120_frames | 2.0469 | 2.8567 | 2.0324 | 1.190 |
| bp/loop_arith_10k | 0.2970 | 0.2972 | 0.2965 | 0.173 |
| bp/tick_1k_instances | 0.0833 | 0.0939 | 0.0828 | 0.048 |
| luau/call_10k | 1.3134 | 1.3288 | 1.3097 | 0.763 |
| luau/loop_100k | 0.9355 | 0.9398 | 0.9283 | 0.544 |
| nav/crowd_1000_agents_30_frames | 59.6380 | 63.9430 | 37.1671 | 34.665 |
| nav/path_across_100_tiles_plain | 0.2111 | 0.2122 | 0.2107 | 0.123 |
| nav/path_across_100_tiles_hierarchical | 0.5674 | 0.5822 | 0.5637 | 0.330 |

## What the numbers say

- **Frame budget.** 1000 navigation agents cost about 2 ms per frame (59.6 ms for 30; the best run was 37 ms, about
  1.2 ms). Iterating 100k entities takes 0.1 ms.
- **Loading.** A 10k-entity binary scene loads in 35.6 ms (about 3.6 microseconds an entity); the JSON form is 1.6x
  slower. A streaming cell of 400 entities decodes in roughly 1.5 ms, so the 35 ms seen in a Debug build earlier was
  about 25 times inflated by the missing optimiser. The hitch budget (42.7) should be judged on the optimised number.
- **Hierarchical path finding** is 2.7x slower than plain Detour on the open 100-tile floor in an optimised build
  (0.57 ms against 0.21 ms for 20 queries), not the 8x seen in Debug. It is still not a win on this mesh.
- **Texture cooking** is the slowest thing measured (BC7 256x256 takes 226 ms), which matters for import times.

## The "regression" was the baseline, not the engine

The same run reported `bp/loop_arith_10k` at 3.61x its checked-in baseline (limit 3.00x), with the step advisory
(`continue-on-error`) so CI stayed green. The old baseline came from a developer machine and a slower build: its
calibration loop took 10.37 ms against 1.72 ms on the runner, a 6x difference, yet `ecs/iterate_100k` was 30x
faster on the runner and `bp/loop_arith_10k` only 1.6x faster (0.477 ms before, 0.297 ms now). The calibration loop
does not scale the same way as the benchmarks across builds, so the normalised comparison flagged a benchmark that
had in fact got faster. Absolute time fell for every benchmark the two files share.

`bench/baseline.json` is therefore regenerated from the table above (the optimised CI build on the runner that
will compare against it), with all 20 benchmarks. To refresh it again: run the `Linux (GCC)` job, copy the table
from its Benchmarks step, and keep `calibration_ms` as the line printed above it.

The step is required. It reported no regression on the ten master runs from 2026-10-15 to 2026-10-22, so a
failure now means a benchmark got more than 3x slower than its baseline after calibration. If a change makes
something deliberately slower, or the runner hardware changes, refresh the baseline as above in the same PR and say
why. The 3.0 tolerance is wide on purpose; tighten it once the spread between runs is measured.

## One baseline row is not a measurement

`ecs/query_50k_of_100k` records `min_ms`, `median_ms`, `max_ms` and `p95_ms` as the same 0.0012 ms across all five
samples. A query over 50 000 entities cannot cost 1.2 µs (24 ps per entity); that is the clock's granularity, so the
row captured the timer floor rather than the work. The same benchmark measures 0.106 ms on a Windows MSVC build, so
a normalised comparison against this row reports a "100x regression" that is pure noise -- the largest of the
false positives in any run, and the reason a local Windows run cannot be compared to the Linux/GCC baseline at all.
Re-record this row on the runner (it is the row to watch when the baseline is refreshed) rather than treating its
ratio as a signal.

## The hierarchical pathfinder does not scale (as measured)

`nav/path_across_100_tiles_hierarchical` used to build a 120 x 120 unit plane and query (-50,-50) to (50,50), with
both endpoints a tile apart: the "hierarchical" case was measuring the same short path as the plain one, under a name
claiming a hundred tiles.

**The tile arithmetic matters here.** A bake tile is `tile_size` cells across and a cell is `cell_size` world units,
so `tile_size = 40` with the default 0.3 cell is a **12-unit tile**, and the bake refuses more than `1 << 14`
(16 384) tiles. The pair is now `nav/path_around_wall_*`: a **1400 x 1400** plane -- ~13 700 tiles, close to the
cap -- with a wall across the middle whose only gap is at the far end, and a query from (-650,-650) to (650,650)
that crosses ~115 tiles. The benchmark aborts if `Stats().direct` is non-zero, so a silent fall back to the plain
path cannot pass as a hierarchical measurement.

Measured on a Windows MSVC build, 20 queries each, on the two plane sizes that were actually run:

| | 480 x 480 plane, open, 1600 tiles | 1400 x 1400 plane, walled, ~13 700 tiles |
|---|---|---|
| plain Detour | 0.70 ms | 15.2 ms (24.1 ms on a second run) |
| hierarchical | 2.53 ms | 583 ms (966 ms on a second run) |
| ratio | 3.6x slower | **38x to 64x slower** |

These are single runs on a noisy developer machine, so treat the absolute numbers loosely and the ratio as the
signal: it went from 3.6x to several tens of times worse as the world grew.

**The gap widens by an order of magnitude with scale**, which is the opposite of what the hierarchy is for. The
likely cause is the portal graph: `FindPath` runs an A* over every portal (`portals_.size()` nodes, allocated per
query as three `n + 1` vectors plus a priority queue) before it makes any leg search, so its cost grows with the
size of the world rather than with the length of the path. At 13 700 tiles that graph search swamps everything.

That made this a **performance defect, not just a benchmark artefact**: a 583 ms path query is unusable in a game
loop, and the feature cost more than the plain pathfinder it is meant to beat.

### First fix: collapse collinear waypoints

Instrumenting the query gave `Stats()`: **55 330 portals, 229 266 edges, only 1 017 nodes expanded**. So the portal
A* was not the cost. The cost was the **leg searches**: the code made one full Detour search per waypoint, and a
route crossing ~115 tile borders produced ~115 of them.

Waypoints that continue in the same direction (within ~2.5 degrees) are now collapsed before the legs run. A
straight run down a corridor needs no intermediate search, and skipping a collinear point is safe because the leg
to the next waypoint is searched properly anyway. On the 13 700-tile benchmark that took the hierarchical query from
**585 ms to 80 ms (7.3x) with an identical path**, and the small serpentine in the unit test went from more than ten
legs to two while still producing the same route.

It is still **4.5x slower than plain** there (80 ms against 18 ms), so the hierarchy does not yet pay for itself.

### The decisive measurement: the case it exists for

`nav/path_through_serpentine_*` adds the scenario a hierarchical pathfinder is built for -- one long route through 19
dead ends on a 1200 x 1200 plane (~10 000 tiles). Measured, 20 queries each:

| | plain Detour | hierarchical | ratio |
|---|---|---|---|
| through the serpentine | 10.59 ms (0.53 ms/query) | 49.81 ms (2.49 ms/query) | **4.7x slower** |

`Stats()` explains it: 36 180 portals, 34 769 nodes expanded, **134 legs**, `direct == 0`. The legs themselves are
cheap -- about 0.02 ms each, roughly 28x cheaper than one plain search -- but a serpentine turns at every gap, so
collinear collapsing barely helps and 134 of them cost more than the single plain search they replace.

**Conclusion: the hierarchical pathfinder is slower than plain Detour in every scenario measured here, including the
one it was designed for.** The break-even needs a world far larger than this branch can bake (16 384 tiles), or a
redesign that makes the route genuinely local rather than a chain of portal-to-portal legs. Until one of those
happens it should not be used for gameplay pathfinding; `NavMesh::FindPath` is the better choice everywhere measured.

### Tried and rejected: merging waypoints by line of sight

The 134 legs suggested merging waypoints whenever `NavMesh::Raycast` showed a clear line from the last kept waypoint
to the next. That halved the legs (134 -> 59 on the serpentine) and made it **slower**, not faster: the serpentine
went from ~50 ms to ~97 ms and a `legs > 2` coverage test started failing. Fewer, longer legs each cost a
correspondingly longer Detour search, so the saved searches buy back less than the merged ones spend. Reverted; the
collinear collapse above is the version to keep.

### Tried and rejected: capping the leg count (which rules out the per-cluster redesign)

If fewer legs are better in principle, the per-cluster portal graph is the obvious redesign. That was tested
cheaply first by subsampling the waypoint list so no query makes more than 20 leg searches, which is the leg count a
per-cluster graph would produce.

It was **5x slower**: the serpentine went from ~66 ms to **288 ms** with 20 legs instead of 134. Each surviving leg
now spans several portals' worth of distance, so its Detour search costs proportionally more, and the total search
area is what the query pays for either way. The hierarchy only wins if a leg is cheap *and* numerous, or short *and*
few -- those are the same thing -- and it cannot be both.

**So the per-cluster redesign would not fix this.** The problem is not how the portals are grouped; it is that a
chain of local searches over the same ground costs more than the one search it replaces, because Detour's own A* is
already efficient on these meshes. Nothing short of eliminating the leg searches entirely (portal A* plus a funnel
over the portal corridor, with no Detour legs at all) has a chance, and that is a different algorithm, not a
tuning change.


