# Large-world coordinates

Status: **Option B started** (UPGRADE_PLAN.md step 42.1). The JSON scene format
records an origin and stores absolute positions today; the binary format and the
cross-system wiring below are still to do. This note records what exists, the
convention decision behind it, and the plan for the systems that still assume
"the world is near the origin".

## The problem

`Transform::position` is `Vec3` (three `f32`). An `f32` has 24 bits of mantissa,
so the representable step is about

| Distance from (0,0,0) | Smallest representable step |
|---|---|
| 1 km | 0.06 mm |
| 16 km | 1 mm |
| 100 km | 8 mm |
| 1 000 km | 62 mm |

Past roughly 16 km, millimetre-scale gameplay (collision margins, attachment
points, animation root motion) starts to jitter; past 100 km, metres-scale
positions are visibly quantised. Nothing in the renderer, physics, audio or
network layer was written with a larger range in mind.

## What exists today

| Piece | Where | What it does |
|---|---|---|
| `Vec3d` | `engine/include/aether/math/vec.h` | double-precision position, arithmetic and `Vec3` conversion |
| `WorldOrigin`, `RebaseOrigin` | `engine/include/aether/math/world_origin.h` | a double-precision anchor; converts absolute `Vec3d` to origin-relative `Vec3` and back; moves the anchor only past a threshold |
| `RebaseTransforms`, `ShiftTransforms` | `engine/include/aether/scene/world_origin.h` | shift **root** Transforms by `-delta`/`+delta`; children are relative to parents; a `Parent` whose GUID no longer resolves counts as a root |
| `SceneLoadOptions::origin` | `engine/include/aether/scene/serialization.h` | after a load, shift root Transforms by `-origin`: a file authored at absolute coordinates comes in at small ones; a JSON scene's own recorded origin is used when this is zero |
| `SceneSaveOptions::origin` | same header | a JSON scene records the origin at its root and writes `position` as `local + origin`, so the loader's shift gets the small values back (`WorldOrigin_JsonSceneRecordsItsOrigin`) |
| `streaming::WorldPosition` | `engine/include/aether/streaming/streamer.h` | a `{f64 x,y,z}` world position used by the streamer |
| `streaming::FloatingOrigin` | same header | applies the shift when a focus point strays past a threshold; now calls `RebaseTransforms` so the rule has one implementation |

These are **additive**: `Transform` and both scene formats are unchanged, and a
system opts in by tracking `Vec3d` and converting through an origin.

## The decision the migration is blocked on

A scene file can store large-world positions two ways. Both are self-consistent;
mixing them is what produces bugs, so the format has to pick one.

### Option A — absolute coordinates in the file (today's behaviour)

The file stores whole-world coordinates; the loader needs to be told the origin
and shifts to local (`SceneLoadOptions::origin`).

- **Pro**: no format change at all; existing files are already this; the origin is
  a runtime concern, so the same file can be loaded at a different origin.
- **Con**: the file itself is `f32`, so a scene authored at 100 km is stored with
  ~8 mm quantisation *before* the loader ever sees it. The damage is done at save
  time and no load-time shift can undo it.

### Option B — origin plus local coordinates in the file

The file records a double-precision origin and stores entity Transforms relative
to it.

- **Pro**: the stored coordinates are the small ones, so nothing is quantised at
  save time; this is what actually preserves precision in large worlds.
- **Con**: a real format change (a new root field and a version bump in both
  `.ascene` and `.aesc`, with a v-old → "origin = 0" migration), and a file is
  now tied to the origin it was authored around unless the loader rebases.

### Recommendation: Option B, with an explicit default

Store the origin and local coordinates. A file written without an origin (every
file today) is `origin = 0` and behaves exactly as now, so the migration is
"v-old loads with origin zero" rather than a data rewrite. `SceneLoadOptions::origin`
then means "override the recorded origin" instead of "declare what the absolute
coordinates were", which is the one semantic flip to document.

Option A is not worth keeping as the primary path: it cannot fix the precision
loss it exists to fix.

## If Option B is chosen, the work is

1. **Format.** *Done for `.ascene`*: `SceneSaveOptions::origin` writes the origin
   at the JSON root and `Transform` positions as `local + origin` (a reflected
   `Vec3` is a three-element array). The binary `.aesc` needs nothing: it is a
   derived format the cooker writes from a scene that is already in local
   coordinates, so its stored positions are the small ones by construction. (A
   tool that wrote `.aesc` straight from absolute coordinates would still need an
   origin field; nothing does.) Old `.ascene` files have no `origin` and load
   with origin zero.
2. **Load.** *Done for `.ascene`*: `LoadSceneJson(..., Vec3d* used_origin)` reports
   the origin it shifted by (the caller's, else the file's), so a game can keep
   the world local and remember where it is (`WorldOrigin_LoaderReportsTheOrigin`).
3. **Physics.** Jolt's broadphase and constraint tolerances are built around
   `f32`; the physics world must be fed origin-relative positions, and
   `RebaseTransforms` must run on the same frame the physics world is rebuilt
   (or the shift applied to the Jolt bodies in place).
4. **Rendering.** The camera-relative path: build the view matrix from
   `camera_absolute - origin`, and submit object transforms already relative to
   the origin. The RHI, shadow cascades and the HLOD proxies from 42.4 all take
   `Vec3`, so they need no type change once the CPU side is relative.
5. **Audio.** Listener and emitter positions to the audio system relative to the
   origin; distance attenuation and Doppler are unchanged at local scale.
6. **Networking.** Replicated positions are the local part plus the origin; a
   client on a different origin needs the server's origin in the snapshot
   header. This interacts with 41.3's relevancy and byte budget.
7. **Tests.** A scene saved at 100 km and loaded back must round-trip to the same
   *absolute* position, and must not drift more than the `f32` step at the
   original origin.

## What must not change

`Transform` stays `f32`. Widening it to `Vec3d` would double the size of the
most-written component, break the archetype layout, the scene format, physics
and the network replication layout at once, and still not give more range than
an origin does. The origin is the cheaper axis: it moves the frame, not the data.

## Verification today

- `Vec3d_PrecisionBeyondFloat` — 1 mm separation at 100 km survives in `f64` and
  collapses in `f32`.
- `WorldOrigin_RebasesIntoRelativeSpace` — conversion and threshold behaviour.
- `WorldOrigin_RebaseShiftsTransforms` — roots move, children do not.
- `WorldOrigin_LoadOptionsShiftTheScene`, `WorldOrigin_BinaryLoadShiftsTheScene`
  — both loaders apply `SceneLoadOptions::origin`.
- `Streaming_FilesAndFloatingOrigin` — the streamer's floating origin.
