# Ropes

The ropes kit adds **ropes, chains and cables**: a rope you can hang a lantern from, a chain that swings, a cable you cut with a script. A `Rope` component on an
entity is the whole setup. The kit is an optional part of the build (`AETHER_KIT_ROPES`, on by default) and needs the physics module. It is built on the physics
module's joints, so a rope is pushed by what it touches and pushes back.

## The component

At the first physics step the rope becomes `segments` small dynamic bodies in a line, `length` long in all, pointing along `direction` from the entity's position.
Each segment is joined to the one before it by a ball-and-socket `PhysicsJoint`, so it bends anywhere. Segments carry a `RopeSegment` component (read-only) with their
index; index 0 is the pinned end.

| Field | What it does |
| --- | --- |
| `length`, `segments` | how long, and how many pieces (2 to 64): more bends more smoothly and costs more |
| `direction` | which way it points from the entity when it is made; down by default |
| `radius`, `mass` | the segments' size (held to 45% of a segment's length so neighbours do not overlap) and the whole rope's mass |
| `damping` | air drag on every segment: a rope with none swings for ever |
| `pin_start`, `start_entity` | the first segment is held where the rope starts: to the world, or to `start_entity`'s body. Off lets the whole rope fall |
| `end_entity`, `end_anchor` | hang this body from the last segment. It needs a `RigidBody`, it gets a `PhysicsJoint` of its own for this (so it must not have one), and `end_anchor` is where on it the rope ties on, which should meet the rope's end when the rope is made |
| `segment_model` | a model drawn for each segment: a cylinder 1 m tall and 1 m across along its local Y axis, scaled to the segment. Empty: invisible |

A rope is built **once, where its entity is when the step finds it**: moving the entity afterwards does not move the rope (pin it to a body for that). Editing the
component while playing takes the rope apart and builds it again. Disabling it, or destroying its entity, removes its segments and the joint it gave `end_entity`.

## Cutting it

`Ropes.Cut` removes the joint between two segments: the part beyond the cut falls away. A rope that has hung still for a while is **asleep** to the physics engine, and
taking a joint away does not wake a body, so the kit wakes every segment (and the end body) when it cuts or lets go: a cut rope never stays hanging in the air.

```lua
Ropes.Cut(rope, 0.5)                       -- cut the middle: 0 is next to the pinned end, 1 next to the free end
print(Ropes.Segments(rope), Ropes.Length(rope))      -- how many pieces; how long the chain is now (its length when unstretched)
local p = Ropes.SegmentPosition(rope, 4)   -- a segment's world position (0 is the pinned end), e.g. to draw something along it
```

## In the player

The player owns the system beside the physics scene and applies it in the physics step, just before the bodies move.

## Limits

A heavy weight on a light rope stretches it: a ball-and-socket chain is a stack of soft constraints, so use more segments and a heavier rope for a heavy load, and
expect some give. The ends of a rope are points (a hinge or a swivel needs a `PhysicsJoint` of your own).
