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

## A renamed benchmark is not a regression

Phase 40.2 replaced the nav path benchmarks, because the hierarchical one measured the wrong thing:
`nav/path_across_100_tiles_plain` and `..._hierarchical` became `nav/path_around_wall_*` and
`nav/path_through_serpentine_*`. The baseline was not refreshed with the rename, so every run afterwards failed with

    can't compare with bench/baseline.json: current report is missing baseline benchmark: nav/path_across_100_tiles_plain

That is a failure about a name, not about speed, and it reads like a missing measurement rather than a missing update. The
two stale rows are removed, so the gate compares the eighteen benchmarks that still exist -- the same eighteen that held for
ten runs before the rename.

Five benchmarks are measured but not yet baselined: the four nav replacements and `physics/step_500_bodies`. They are reported
and not gated until the next refresh, which has to happen on the `Linux (GCC)` runner as above. A baseline written anywhere
else is worse than none: this machine's calibration-normalised times are 100x off on `ecs/query_50k_of_100k`, so refreshing
from it would fail the runner on every benchmark at once.

**A rename must refresh the baseline in the same commit.**

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

### Tried and rejected: capping the leg count (rules out per-cluster legs, not per-cluster A*)

If fewer legs are better in principle, the per-cluster portal graph is the obvious redesign. That was tested
cheaply first by subsampling the waypoint list so no query makes more than 20 leg searches, which is the leg count a
per-cluster graph would produce.

It was **5x slower**: the serpentine went from ~66 ms to **288 ms** with 20 legs instead of 134. Each surviving leg
now spans several portals' worth of distance, so its Detour search costs proportionally more, and the total search
area is what the query pays for either way. The hierarchy only wins if a leg is cheap *and* numerous, or short *and*
few -- those are the same thing -- and it cannot be both.

**So a per-cluster graph would not fix the legs.** The cost is not how the portals are grouped; it is that a chain of
local searches over the same ground costs about what the one search it replaces costs, because Detour's own A* is
already efficient on these meshes. Any number of legs whose total span covers the route pays roughly the route.

**Be precise about what this does and does not rule out.** It rules out the legs. It does not rule out the *other*
half of the cost, which this run did not isolate: the portal A* itself, which expanded 34 769 nodes over 36 180
portals and 229 266 edges on the serpentine.

`HierarchicalStats` now records `graph_ms` and `legs_ms` per query, which settles it. On the serpentine, per query:

| | ms | share |
|---|---|---|
| portal A* | **2.27** | **75%** |
| Detour legs (134 of them) | 0.72 | 24% |
| total | ~3.0 | |
| plain Detour, for comparison | 0.54 | |

**The A* is the cost, not the legs.** So a funnel over the portal corridor -- which removes the leg searches
entirely -- would save at most the 0.72 ms and leave a query about four times slower than plain Detour. The funnel is
**not** the fix on its own.

What the numbers do support is the combination: a **coarser graph** (portals per cluster rather than per tile border,
shrinking the A* by roughly the cluster area) **plus** a funnel to turn the portal sequence into a path with no
Detour legs at all. That needs the A* to fall from 2.27 ms to well under 0.54 ms -- about a 4x reduction in nodes --
which a clustered graph of the right size could plausibly deliver. It is a different algorithm and remains
unmeasured; but it is now a scoped, falsifiable next step rather than a guess.

### Why the A* cannot be tuned: it degenerates to Dijkstra on the geometry it is for

`graph_nodes_expanded` is **34 769 of 36 180 portals -- 96%**. The A* is expanding almost the entire graph, which
means its heuristic is buying nothing there. That is not a bug in the heuristic: `h` is straight-line distance to the
goal and it is admissible, but on a serpentine the walkable route is many times the straight line, so `h`
underestimates by an order of magnitude and the search collapses to Dijkstra.

The contrast makes the point: on the walled 13 700-tile mesh the same A* expands **1 017 of 55 330 -- 2%** -- a mild
detour, an informative heuristic, a cheap query. A hierarchical pathfinder exists for the *serpentine* case, and that
is exactly the case where its own graph search becomes uniform-cost.

So the only lever left is the one already named: fewer nodes, not faster nodes. Whether a clustered graph's
coarser portals still yield a valid route once a funnel connects them is the open, measurable question.

### The lever works: a coarser graph shrinks the A* enough to matter

That lever was then measured directly, with no code change: bake the same serpentine on a coarser tile grid
(`tile_size` 120 instead of 40, i.e. 36-unit tiles instead of 12), which gives the same geometry with far fewer
portals.

| serpentine, per query | `tile_size` 40 | `tile_size` 120 |
|---|---|---|
| portals | 36 180 | **4 908** (7.4x fewer) |
| A* nodes expanded | 34 769 | **4 451** |
| `graph_ms` | 2.27 | **0.37** |
| `legs_ms` (134 / 75 legs) | 0.72 | 0.84 |
| hierarchical total | ~3.0 | **1.31** |
| plain Detour | 0.54 | **0.43** |

The A* falls 6x for 7.4x fewer nodes -- the lever is real and close to linear. And the balance flips: the **legs**
(0.84 ms) now cost more than twice the A* (0.37 ms), whereas before the A* dominated.

**So the clustered-graph-plus-funnel design does have a measured path to winning**, which earlier revisions of this
note wrongly suggested it did not: at this graph size the A* alone (0.37 ms) is already below plain Detour on the
same mesh (0.43 ms), and a funnel replaces the 0.84 ms of leg searches with a linear walk over the portal corridor.
The margin is thin -- roughly 0.06 ms if the funnel were free -- so what decides it is how coarse the graph can get
before the funnel can no longer produce a valid route, and what the funnel itself costs. That is the next measurement,
and it needs the algorithm written; unlike the previous three ideas this one is supported by numbers before it is
built.

### What the funnel implementation has to get right

Written down from reading `Build`, so the next attempt does not rediscover it:

- **`Portal` only stores the edge midpoint** (`point`). A funnel needs the edge's two endpoints, so `Portal` gains
  `left`/`right` and `Build` fills them from `tile->verts[poly.verts[j]]` and `[(j + 1) % poly.vertCount]` -- both are
  already in hand at the point the portal is created.
- **Their order is not the order the funnel wants.** `(a, b)` comes from the polygon winding, which is consistent per
  polygon but not relative to the direction of travel along the route. Each portal's pair must be reordered so the
  corridor runs left-to-right with respect to travel -- e.g. from the cross product of the two endpoints against
  `via[i + 1] - via[i - 1]`. Getting this wrong yields paths that look plausible and cut through walls, and a
  length-based test will not catch it; the check has to be that every output segment is walkable (`NavMesh::Raycast`
  between consecutive path points).
- **The leg loop is replaced, not augmented.** The A* currently returns portal indices through `from[]`; the funnel
  consumes those same indices. Nothing else in `FindPath` changes, and the `direct()` fallback stays.
- **The bake has to stay at the coarser tile size** for the win to exist, so the benchmark's `Serpentine()` should use
  `tile_size` 120 when measuring, or the comparison is against the slower configuration.

Only after those are in place is "A* + funnel < 0.43 ms" a meaningful target.

### Tried and rejected: raycast string-pull instead of legs

Before writing the funnel, the legs were replaced with something simpler and self-validating: keep the portal
midpoints as a coarse route and string-pull them with `NavMesh::Raycast`, keeping a segment only when the ray is
clear. No Detour searches at all, and walkable by construction rather than by assumption.

It won in one case and lost badly in the other:

| 20 queries | plain | hierarchical (string-pull) |
|---|---|---|
| walled route, 13 700 tiles | 15.19 ms | **13.84 ms** -- the first time it beat plain |
| serpentine, 10 000 tiles | 10.92 ms | **476 ms** |

The serpentine collapsed for a structural reason: the probe searches the *entire* remaining route for a clear ray,
which is O(n^2) raycasts -- about 10 000 per query, 20.3 ms of it. A bounded probe would fix the complexity but not
the other failure: the string-pulled serpentine path came out **shorter than the walkable route**, so it was cutting
corners the corridor does not allow, and the `Length > 1450` assertion caught it.

So: the idea is sound for short routes (the walled case is worth revisiting if the portal graph ever shrinks), but it
is not a substitute for a funnel on long ones. Reverted; the collinear collapse is still the version to keep.

**This does leave one genuinely encouraging data point.** In the walled case the hierarchy beat plain Detour with no
tuning -- 13.84 against 15.19 ms -- which is the first measurement in this whole investigation where it came out
ahead. The feature is not hopeless; it is wrong for the long, dead-end routes it was designed around.

**And that encouraging result was not real.** Repeat attempts with a bounded probe (eight portals ahead, so every ray
stays short) made the walled case **4.05 ms against plain's 15.03** -- 3.7x faster, which is far too good. The
serpentine test said why: the path came out shorter than `Length > 1450` allows, so it was cutting through walls.
The speed was the symptom, not the achievement: a pathfinder that skips the walls is fast because it does less work,
not because it is better.

That is the trap to keep in mind with every number in this section: **`nav/path_*` benchmarks only assert that a path
was returned, not that it is walkable.** The `Length` assertions in `test_nav_hierarchical.cpp` are what caught both
string-pull attempts.

### And the obvious fix for that does not work either

The natural repair -- assert walkability by raycasting every segment -- was tried in the benchmarks and **aborts on
plain Detour's own serpentine path** (`nav path segment 3 of 12 is blocked`), for both the plain and the hierarchical
benchmark. A check that rejects the reference implementation's output is not a check. `NavMesh::Raycast` reports
"blocked" for segments that clip a wall corner, which Detour considers valid, so it cannot be used as a validity
oracle without a tolerance that nobody has established.

That also retroactively undermines the earlier claim that the string-pull was "walkable by construction": its
validity rested on exactly this raycast, so it was never actually verified -- the `Length` assertion is the only
evidence it was wrong, and the only trustworthy check this investigation found. Until a better validity oracle
exists, path correctness here is measured by path *shape* (the unit tests), not by segment raycasts, and a smoothing
change must be judged by those tests rather than by a benchmark that only times it.

**One real bug did come out of that detour and is fixed.** `NavMesh::Raycast` returned `false` -- "not blocked" --
when Detour's raycast *failed*, most often because the ray crossed more polygons than `kMaxPathPolys` allows. That is
a false clear, and it is what let the string-pull straighten a path through a wall: long rays came back clean. It now
reports a failed query as blocked, which is the conservative answer for a line-of-sight test. The remaining
unreliability is in the other direction (corner-clipping segments read as blocked), which is why the segment check is
still not usable as a validity oracle.

**That fix has no test, and the reason is worth writing down.** Reaching the failure path needs a ray crossing more
than 512 polygons, and no testable mesh produces one: a flat surface merges into a handful of polygons per tile
however fine the voxels, and a surface fragmented with shallow ridges merges too -- both were tried and the raycast
succeeded in both. There is no cheap way to make Detour's own query run out of budget, so the behaviour is covered by
the API comment in `navmesh.h` rather than by a regression test. If the budget or the tessellation ever changes, that
is the case to re-check first.

### Kept: abandon the graph search when it degenerates

The 96%-expansion finding has a cheap consequence, and it is the one change from this investigation that is worth
keeping. `FindPath` now counts expansions against a budget of `max(512, portals / 16)` and, if the A* blows through
it, hands the query to the plain pathfinder. A 5% expansion rate means the heuristic has stopped buying anything, so
finishing the graph walk (and then paying for the legs) can only lose.

| 20 queries | plain | before | after |
|---|---|---|---|
| serpentine | 11.19 ms | 65.61 ms | **15.81 ms** |
| walled route (mild detour, unaffected) | 15.41 ms | 67.29 ms | 67.29 ms |

The serpentine goes from 4.7x slower than plain to 1.4x, and `HierarchicalStats::degenerate` counts the queries that
took the fallback so the behaviour is visible rather than implicit. All 1 432 engine tests pass.

It does not make the pathfinder worth using -- it is still slower than plain Detour in every scenario measured --
but "falls back instead of thundering ahead" is the correct behaviour for a pathfinder with a heuristic it cannot
trust, and it costs one comparison per expansion.



## Baseline refresh, 7 November 2026

The `Linux (GCC)` lane failed with `REGRESSION ecs/query_50k_of_100k: 30.73x the baseline (limit 3.00x)`. That row's
baseline was 1.2 us normalised to 0.0007, which no implementation of a query over 100k entities can reach; the runner
measured 30 us, normalised 0.021. Every other shared row came in between 0.45x and 1.01x of its baseline, so the engine
had not slowed down -- one row's baseline was wrong.

`bench/baseline.json` was regenerated from that run's own table, which is the procedure this document prescribes:
calibration 1.412 ms, 22 rows instead of 18. The four added rows are the current navigation benchmarks, which the file
had never carried under these names; the two it did carry (`nav/path_across_100_tiles_plain` and its hierarchical twin)
no longer exist and had already been removed. Only `name` and `normalized` are compared, so the other fields are
informational.

### Why that one row was 30x: the benchmark got honest, not slow

The source of the bad baseline is now known, and it is not a mystery of calibration. That row's body **changed** in
`bacf2dc4` ("Engine: fix D3D12 texture crash, tune ECS/job hot paths"): it gained an accumulated sum and an `abort` guard,
with the comment *"observe the data so the loop cannot be elided"*. Before that, the loop only counted matching rows, which
the optimiser collapsed into per-chunk arithmetic and reported at 0.001 ms -- the benchmark was measuring nothing.

So the 30x is not a slowdown at all. It is the cost of doing the query, measured for the first time. The baseline was
recorded before the benchmark became honest, and every subsequent comparison was against a number that could not be
reached by any correct implementation. **A changed benchmark invalidates its baseline in the same way a renamed one does**,
and that rule is the reason this cost so much: the guard landed without a baseline refresh in the same commit.
### The benchmark gate's own noise, 7 November 2026

After the baseline was refreshed from the runner, the next run still failed -- on different rows. The report artifact from
that run is the evidence, and it says the comparison is measuring the runner, not the engine:

| Row | Baseline | This run | Ratio |
|---|---|---|---|
| `ecs/query_50k_of_100k` (the row that started this) | 0.0210 | 0.0312 | **1.49x** -- fixed |
| `bp/loop_arith_10k` | 0.1330 | 0.4391 | 3.30x |
| `cook/atex_roundtrip_256` | 0.0010 | 0.0126 | 12.61x |

Two things are visible in the full table. First, **every** row came in between 0.95x and 1.66x of the refreshed baseline:
the whole run was slower, consistently. The calibration loop took 1.97 ms against 1.41 ms in the run the baseline was
taken from, a 40% difference that normalisation only partly removes. Second, `bp/loop_arith_10k` measured 865 us with a
min-to-p95 spread of **5 us** -- a tight, repeatable measurement of a slower run, not noise within the run. That row is
also the one this document already recorded at 3.61x in a previous round, where it had in fact got faster.

So the conclusion is not "refresh the baseline again". Each refresh moves the target to whichever conditions that run
happened to see, and the next run drifts away from it. The gate compares two different machine states with a 3x tolerance
and calls the difference a regression.

`docs/design/M8_PRODUCTION_TECHNOLOGY.md` -- the milestone this work belongs to -- says: *"Shared-runner performance
thresholds should remain advisory until their noise and repeatability are understood."* That is exactly the state now:
noise measured, repeatability poor across runs, threshold still required. The options are a noise floor for
micro-benchmarks, a per-runner baseline, or advisory status; **all three are policy decisions about the gate, not edits to
a number**, and none of them should be made by quietly moving the baseline again.
### The calibration loop is not a machine proxy, 7 November 2026

The benchmark now prints its own diagnosis when a comparison fails: the calibration figure from each report, how many
shared benchmarks moved each way, and a warning when nearly all of them moved together. It exists because reading that
out of a 22-row artifact table by hand is what this round cost.

Running it on this machine produced the number that matters more than any CI run:

```
calibration 1.41 ms in the baseline, 1.42 ms now (1.00x)
22 of 22 shared benchmarks slower, 0 faster
the whole run is slower, not one benchmark: check the runner before the code
```

**The calibration loops agree to within 1% and all 22 benchmarks are 3x to 7x slower.** So the calibration loop -- the
thing the whole normalised comparison divides by -- does not capture the difference between these two machines at all.
It measures a tight arithmetic loop; the benchmarks measure memory traffic, allocation and branch behaviour. Dividing one
by the other does not cancel hardware, it hides it behind a number that looks like a correction.

That is why the gate fails on a different pair of rows each run, and why refreshing the baseline fixes exactly the rows it
was taken from and no others. A per-runner baseline or a noise floor would both help; neither fixes the underlying point
that this normalisation cannot compare two machines.
### Recommendation for the gate, 7 November 2026

Three options were on the table; the measurements above rule one out and rank the others.

**Not a noise floor.** The failing rows are not micro-benchmarks: `bp/loop_arith_10k` measured 865 us with a 5 us
min-to-p95 spread, and `ecs/query_50k_of_100k` 30 us. A floor would have to be set above a millisecond to silence them,
which would silence most of the table.

**Not another baseline refresh.** It fixes exactly the rows it was taken from, and the next run drifts. This was done
once already in this round and the failure moved to two different rows.

**Recommended: a baseline per hardware class, with the comparison advisory until the class is known.** The calibration
loop cannot stand in for the machine -- 1% agreement and a 3x-to-7x spread across the table on this machine -- so the
baseline has to be tied to the hardware that produced it, and a runner without its own baseline should report rather
than fail. That is also what `M8_PRODUCTION_TECHNOLOGY.md` already says: *shared-runner performance thresholds should
remain advisory until their noise and repeatability are understood.*

Whichever is chosen, the change is to the gate's policy, and the `bench --baseline` output now carries the evidence
needed to review it: both calibration figures, how many benchmarks moved each way, and a warning when the run moved as a
whole.
### A failed comparison is re-measured once, 7 November 2026

Applied with the recommendation above, and deliberately not a weakening of what the gate is for. The `Benchmarks` step
now re-runs the comparison once when the first measurement fails, and the lane only fails if **two consecutive
measurements** disagree with the baseline. The second report is the artifact, so a real regression is still visible twice
over rather than being talked away.

The justification is the measurement, not convenience: two consecutive runs on this runner differed by 40% in the
calibration loop with every benchmark moving the same way, so a single shared-runner measurement is not evidence of a
regression on its own. Every benchmark that is genuinely slower still shows up in both runs, and the diagnostics added to
`bench --baseline` print the calibration pair and the slower/faster counts so that a reviewer reads the cause in the log
rather than inferring it from an artifact.

One measurement was never enough; it only looked like enough while the runner happened to be stable for ten runs.

### Refined: one unstable micro-benchmark, not a broken gate, 7 November 2026

The re-measure experiment answered the question it was built for. The next CI run also failed the benchmark step, which ruled
out "one noisy reading" -- so the report artifact was read again, and it says something much narrower than the earlier
conclusion:

- **1 row of 22** was over the 3x tolerance: `cook/atex_roundtrip_256` at 11x.
- The median row was **1.21x**, and that run's calibration loop was **1.60x** the baseline's, so the run was simply slower
  overall and normalisation roughly matched it.
- That one row's **baseline median is 1.8 microseconds** and it measured 25 microseconds. A 1.8 us benchmark is at the timer
  floor: it cannot distinguish 2 us from 25 us reliably, and across three runs of the same job it swung between 0.001 and
  0.139 normalised -- a 139x spread.

So the fix is not a per-runner baseline and not an advisory gate. It is a **noise floor**: `aether_bench --baseline` accepts
`--noise-floor-ms`, and a benchmark whose baseline median is below it is **measured and reported but not failed**. CI passes
0.01 ms, which excludes the 1.8 us row and keeps everything with a baseline at or above `ecs/query_50k_of_100k`'s 30 us
under the gate.

Verified by running the comparison twice locally: `cook/atex_roundtrip_256` is reported as a regression without the floor and
is not reported with it.

## CPU skinning cost, 8 November 2026

`player/src/scene_renderer.cpp` skins on the CPU: every vertex is blended against its joint matrices each frame and the
vertex buffers are rewritten. `bench/src/bench_skinning.cpp` times that loop (position, normal and tangent over four
influences, normalise, re-orthogonalise the tangent; now `assets::SkinVertices`) to answer one question: is it a bottleneck?

Windows, MSVC RelWithDebInfo, one thread, synthetic meshes. **The median is one frame of skinning.**

| Benchmark | Median per frame | Share of a 16.6 ms (60 fps) frame |
|---|---|---|
| 104 vertices x 1 (the shipped AETHER-01 Kael rig) | 0.007 ms | 0.04% |
| 20,000 vertices x 1 (a hero) | 1.19 ms | 7% |
| 5,000 vertices x 10 (an NPC group) | 2.58 ms | 16% |
| 20,000 vertices x 10 (ten heroes) | 13.3 ms | 80% |
| 5,000 vertices x 100 (a crowd) | 29.0 ms | 175% |

That is about **60 ns per vertex**, and it scales linearly with vertices times characters.

What this says:

- **Not a bottleneck for AETHER-01 as shipped.** Kael has 104 vertices and Warden 112, so skinning is noise there. Do not
  build GPU skinning to fix AETHER-01.
- **It is a bottleneck at AAA character counts.** One hero is cheap, but ten heroes use four fifths of a 60 fps frame
  and a 100-NPC crowd does not fit in the frame at all, before rendering, physics or gameplay.
- **Caveats.** Single-threaded; synthetic vertices and weights; the loop only, so pose evaluation and the vertex-buffer
  upload are not included. The per-frame upload will add to these figures. Real production rigs also have far more
  joints, which makes matrix and cache behaviour worse, not better.

Decision: GPU skinning is justified by the AAA target, not by the proof game. It should come with a test that renders a
deformed pose, so it lands together with the evidence A06 is missing.

### Making it faster: one idea did not work, one did (8 November 2026)

**Tried and rejected: blend the four joint matrices once per vertex.** The idea was to build one 3x4 and one 3x3 matrix
per vertex (sum of weight times matrix) and transform position, normal and tangent by those, instead of transforming each
by every influence: roughly half the multiplies on paper. Measured against the original loop on the same meshes:

| Case | Original loop | Blended matrices |
|---|---|---|
| 20,000 vertices x 1 | 0.93 ms | 0.96 ms |
| 20,000 vertices x 10 | 10.3 ms | 10.7 ms |
| 5,000 vertices x 100 | 26.3 ms | 31.4 ms |

It was no faster, and slower on the small meshes. The original loop is already compiled into vector operations on
`Vec4`/`Mat4`, so the "saved" multiplies were not what the time was going to; the blended version replaced that with
scalar code. It matched the original to float rounding, and it was removed rather than kept as dead complexity.

**Kept: split each skinned mesh across the job system.** `SkinVerticesParallel` runs chunks of at least 1,024 vertices
on a small `JobSystem` (two workers plus the rendering thread). The output is bit-identical to the serial loop, because each
vertex is computed the same way and only the order of work changes. The player uses it for primitives of 4,096 vertices
or more and skins smaller ones in place, where a fork-join would cost more than it saves.

| Case | Serial | Parallel | Speed-up | Share of a 16.6 ms frame, parallel |
|---|---|---|---|---|
| 104 vertices x 1 (Kael) | 0.006 ms | not used (below the threshold) | | |
| 20,000 vertices x 1 | 0.97 ms | 0.28 ms | 3.4x | 2% |
| 20,000 vertices x 10 | 10.4 ms | 2.76 ms | 3.8x | 17% |
| 5,000 vertices x 100 | 25.1 ms | 9.7 ms | 2.6x | 58% |

Timer noise between runs of the same binary on this machine was about 25% (the first table in the previous section was
taken on a different run), so read these as ratios within one run, not as absolute numbers.

What this does and does not settle:

- Ten production heroes now fit comfortably. A 100-NPC crowd of 5,000-vertex characters still takes over half a frame,
  and that is skinning alone. Splitting work per character, instead of per mesh, would help the crowd case (a 5,000-vertex
  mesh only splits into a few chunks); it has not been done.
- This is still CPU work plus a vertex-buffer upload every frame. GPU skinning would remove both, but needs a new vertex
  layout and a joint-palette binding in the graphics RHI on D3D12 and Vulkan. It is not justified by AETHER-01 (104 vertices)
  and not yet by these numbers for a single hero; revisit it when a real scene has a crowd.
