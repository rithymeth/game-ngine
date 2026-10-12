# Release benchmark baseline (Windows, MSVC)

Three full passes of `aether_bench --repeat 15` against one Release build, one after another on an otherwise idle machine. The medians below are the
median of 15 samples inside each pass; the spread column is how far the three passes' medians are from each other, so it is the number to compare a
future change against.

| | |
|---|---|
| Machine | Intel Core i7-11800H (8 cores, 16 threads), 15.7 GB RAM, Windows 11 |
| Compiler | MSVC 14.44 (VS 2022 Build Tools), `-DCMAKE_BUILD_TYPE=Release` |
| Source | commit `bfb7470` plus the uncommitted working tree of PR #198 (the benchmarks themselves are unchanged by it) |
| Job system | 15 workers + the calling thread |
| Calibration loop | 1.59 to 1.66 ms across the passes |

## Results (median ms)

| benchmark | pass 1 | pass 2 | pass 3 | spread |
|---|---:|---:|---:|---:|
| ecs/iterate_100k | 0.1794 | 0.1707 | 0.1591 | 11.9% |
| ecs/query_50k_of_100k | 0.0227 | 0.0226 | 0.0224 | 1.5% |
| ecs/create_destroy_10k | 1.3481 | 1.3736 | 1.3882 | 2.9% |
| ecs/add_remove_component_10k | 2.1426 | 2.1397 | 2.2683 | 5.9% |
| jobs/batch_1024 | 0.0965 | 0.1007 | 0.0983 | 4.3% |
| scene/save_binary_10k | 55.6192 | 57.4482 | 58.8600 | 5.7% |
| scene/load_binary_10k | 78.0413 | 82.6518 | 81.7775 | 5.7% |
| scene/save_json_10k | 81.7235 | 87.3364 | 86.1484 | 6.6% |
| scene/load_json_10k | 117.9147 | 117.1707 | 117.3300 | 0.6% |
| cook/texture_bc7_256 | 225.4469 | 224.4257 | 225.1784 | 0.5% |
| cook/texture_bc1_512_mips | 174.7916 | 174.3304 | 174.3531 | 0.3% |
| cook/atex_roundtrip_256 | 0.0076 | 0.0076 | 0.0078 | 2.7% |
| **world/flythrough_120_frames** | 2.5829 | 2.6144 | 2.6108 | 1.2% |
| bp/loop_arith_10k | 0.2769 | 0.2774 | 0.2780 | 0.4% |
| bp/tick_1k_instances | 0.1299 | 0.1274 | 0.1262 | 2.9% |
| luau/call_10k | 2.6871 | 2.6827 | 2.6931 | 0.4% |
| luau/loop_100k | 0.9651 | 0.9270 | 0.9482 | 4.0% |
| **nav/crowd_1000_agents_30_frames** | 43.6743 | 43.5246 | 44.0385 | 1.2% |
| nav/path_across_100_tiles_plain | 0.2159 | 0.2139 | 0.2150 | 0.9% |
| nav/path_across_100_tiles_hierarchical | 0.8720 | 0.8350 | 0.8559 | 4.3% |

## The two asked about

- **1,000 agents** (`nav/crowd_1000_agents_30_frames`): about 43.7 ms for 30 frames, **1.46 ms a frame**, about 9% of a 16.7 ms (60 fps) frame, and
  repeatable to 1.2% between passes. The p95 of the first pass was 77.5 ms against a 42.4 ms minimum, so an occasional pass is slower: look at the
  median and the minimum, not one run.
- **Streaming** (`world/flythrough_120_frames`): about 2.6 ms for 120 frames (**0.022 ms a frame**) of a camera crossing the generated 40 x 40 block
  test world and gathering what is in range, repeatable to 1.2%. This is the CPU side of a streaming pass only (the range gather). It does not load or
  unload cells, and it does not measure render hitches, which need hardware; the benchmark's own comment says so. A benchmark that loads and unloads
  world-partition cells through the streaming tools has not been written.

## How far to trust the numbers

The three passes agree with each other to a few percent because they ran back to back. A fourth, separate run of only `nav/crowd_1000_agents_30_frames`
(`--repeat 7`, a few minutes later) measured a **32.7 ms median and a 24.6 ms minimum**, about 25% faster than the 43.7 ms of the three passes, with
the calibration loop at 1.50 ms against 1.59 to 1.66 ms. On a laptop CPU, clocks and background load move a benchmark of this size by more than the
spread inside one session. So:

- compare a change against a pass taken in the same sitting, ideally alternating before and after;
- prefer the minimum and the normalized figure over a single median;
- treat a difference under about 25% on the long benchmarks (`nav/crowd`, the `scene/` and `cook/` rows) as noise unless it repeats;
- the one-session spread of the short benchmarks (`ecs/iterate_100k` 11.9%) is already large because each sample takes a fraction of a millisecond.

`--baseline` compares a whole report, so it cannot be combined with `--filter` (a filtered run is missing the other benchmarks and the comparison is
refused).

## Reproducing

```
cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release -DAETHER_BUILD_BENCH=ON -DAETHER_BUILD_TESTS=OFF -DAETHER_BUILD_MCP=OFF
cmake --build build_release --target aether_bench
build_release/bench/aether_bench.exe --repeat 15 --json run.json
build_release/bench/aether_bench.exe --repeat 15 --baseline docs/perf/run1.json --tolerance 1.5
```

`run1.json`, `run2.json` and `run3.json` in this folder are the three passes. The `--baseline` comparison divides each time by the run's own
calibration loop, so another machine can be checked against them, but absolute milliseconds are only comparable on the same hardware.
