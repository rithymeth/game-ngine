# Physics constraints

Status: v1 (engine API). Consumer: Ghost Shift (braced doors, the containment
tether, two-person carrying, breakable props), but nothing here is
game-specific.

Code: `physics/include/aether/physics/constraints.h`,
`physics/src/constraints.cpp`, the constraint section of
`physics/include/aether/physics/physics_scene.h`, Luau bindings in
`scripting/src/script_api.cpp`, tests in `tests/test_physics_constraints.cpp`.

## Pieces

| Piece | What | Lives in |
|---|---|---|
| `PhysicsJoint` | Authored joint from an entity's body to `connected` (EntityGuid; null = world) | ECS component, reflected v1, saved/cooked |
| Grab | Runtime pull of a body point toward a target by a holder | `PhysicsScene::Grab` / `SetGrabTarget` / `Release` / `ReleaseHolder` |
| `ConstraintBreakEvent` | A joint, lock or grab broke | `SetConstraintBreakHandler`, `ConstraintBreaks()` |
| `ConstraintSnapshot` | Joint live state + jointed/grabbed body poses for networking | `CaptureConstraintState` / `ApplyConstraintState`, `Serialize`/`DeserializeConstraintSnapshot` |

### PhysicsJoint

Types: `Fixed`, `Point`, `Hinge` (axis, limits `min_angle <= 0 <= max_angle`
in degrees, optional `Velocity`/`Position` motor with `motor_max_torque`,
`motor_frequency`, `motor_damping`), `Distance` (`min_distance`,
`max_distance` with negative = length at creation, `connected_anchor`,
optional limit spring `spring_frequency`/`spring_damping`).

Shared: `break_force` (N), `break_torque` (N m), `lock_break_force`
(force or torque carried by the lock). 0 = unbreakable.

Live fields, applied without rebuilding the Jolt constraint:

* `enabled` - Jolt `SetEnabled` on the joint and its lock.
* `locked` - adds a rigid `FixedConstraint` between the same bodies at the
  current pose (a braced door). Past `lock_break_force` only the lock breaks
  (`locked` is cleared, a `Lock` event fires) and the hinge keeps working.
* `broken` - set by a break (the constraint and its lock are removed). Clear it
  to re-arm: the joint is rebuilt at the bodies' current poses.
* `motor_target` - hinge degrees (Position) or degrees/s (Velocity).
* `angle` - read back each step: hinge degrees, distance metres (0 otherwise).

`enabled`, `locked`, `broken` and `angle` are `Field_Replicated`.

Jolt setup: body 1 = connected body (or `Body::sFixedToWorld`), body 2 = the
joint's entity. All joints are created in world space from the bodies' poses at
creation; that pose is hinge angle 0. A joint needs at least one non-static
body.

### Lifetime (PhysicsScene)

* `Sync` (also run by `Step`) resolves `connected` through IdComponents and
  creates the constraint once both bodies exist.
* Settings change (anything but the live fields), a rebuilt body, or a
  different connected entity -> the constraint is removed and recreated.
* Component removed, entity destroyed, connected entity destroyed -> removed
  (a joint whose connected entity is missing waits, and connects if it
  reappears).
* Constraints are always removed before their bodies: `Destroy` of a body
  first detaches its joints and grabs, and `~PhysicsScene` removes every
  constraint before destroying bodies.

### Grabs

`Grab(object, holder, world_point, GrabSettings)` adds a `SixDOFConstraint`
from the world to the object's (dynamic) body. Translation axes are free with
position motors (spring `frequency`/`damping`) whose force is capped at
`max_force`; rotation is free. Each holder is a separate constraint, so holders'
forces add: two 300 N holders lift a 50 kg crate (490 N) that one cannot.
`SetGrabTarget` moves the motor target. A grab ends on `Release`,
`ReleaseHolder`, a break (`break_force` reached by the motor, or the point more
than `break_distance` from the target), or its body going away.

### Breaks

After each physics step, constraint loads are read from Jolt's accumulated
impulses (lambda / dt). Order is deterministic: per joint by entity index the
lock then the joint, then grabs by id. Each break is reported once
(`ConstraintBreakEvent{kind, entity, other, grab, force}`) to the break handler
and in `ConstraintBreaks()` for that step; the broken constraint is removed so
it can't fire again until re-armed.

### Luau

`Physics.Grab(object, holder, point, maxForce?, frequency?, damping?, breakForce?)`
-> grab id (0 = failed), `Physics.SetGrabTarget(grab, point)`,
`Physics.Release(grab)`, `Physics.IsGrabActive(grab)`,
`Physics.SetJointEnabled(entity, bool)`, `Physics.SetJointLocked(entity, bool)`,
`Physics.SetJointMotorTarget(entity, number)`,
`Physics.GetJointState(entity)` -> `enabled, locked, broken, angle` (nil without
a joint), `Physics.RepairJoint(entity)` -> whether it was broken.

## Networking (GS-04 notes)

Model: **host-authoritative constraints**. The host runs gameplay (grabs,
motor targets, locks) and the authoritative simulation; clients simulate the
same scene for smoothness and are corrected from snapshots.

Snapshot (`ConstraintSnapshot`): every joint's live state (`enabled`, `locked`,
`broken`, `angle`, `motor_target`) keyed by the owner's GUID, plus position,
rotation, linear and angular velocity of every jointed or grabbed body with an
IdComponent, keyed by GUID, both sorted by GUID. Wire format (little-endian):

```
"AECS" u16 version=1
u32 joint_count, joint_count x { u64 hi, u64 lo, u8 flags(enabled|locked<<1|broken<<2), f32 angle, f32 motor_target }
u32 body_count,  body_count  x { u64 hi, u64 lo, f32 pos[3], f32 rot[4], f32 linvel[3], f32 angvel[3] }
```

`DeserializeConstraintSnapshot` rejects wrong magic/version, truncated or
trailing bytes, unknown flags and non-finite numbers.

`ApplyConstraintState` writes the joint fields and teleports the bodies with
their velocities (also updating Transforms); joint changes take effect at the
next `Step`. Snapshots are idempotent and order-free per packet, so dropped
packets only delay convergence (tested: snapshots every 3 steps, 25% loss,
client within 1e-4 m of the host).

Determinism: two worlds built and driven identically are bit-identical over
240 steps and produce identical snapshots (same binary, same Jolt build).
Constraints are added in a deterministic order (entity index, grab id).

### Left for GS-04

* Wire the snapshot into the net layer (channel, rate, unreliable send of the
  latest snapshot; relevance filtering by distance/interest).
* Client-side grab prediction: clients don't create grabs; a client's own
  hold should be predicted locally and reconciled.
* Interpolation/smoothing of the correction instead of a hard teleport.
* Host-side validation of grab requests (reach, max holders per prop).
* Joint reference pose: a joint created on a late-joining client is set up at
  the poses it sees then; scenes should be loaded in their authored pose
  before snapshots are applied (or the reference must be authored).
* Break events are host-only; clients learn of them through `broken`/`locked`
  in snapshots (replicate a gameplay event for effects).
