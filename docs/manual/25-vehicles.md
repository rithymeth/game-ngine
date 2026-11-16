# Vehicles

The vehicles kit adds **wheeled vehicles**: a car with four wheels on springs, an engine, an automatic gearbox, brakes and a handbrake, built on Jolt's vehicle
constraint. A `Vehicle` component on a dynamic body with a box collider is the whole setup. The kit is an optional part of the build
(`AETHER_KIT_VEHICLES`, on by default) and needs the physics module.

## The component

Put `Vehicle` on an entity that has a dynamic `RigidBody` and a `BoxCollider` for the chassis. The car drives along the entity's **+Z** axis (its nose) with +Y
up. The wheels are placed from the collider, in the order front-left, front-right, rear-left, rear-right: `track_fraction` of the chassis' half width out and
`wheelbase_fraction` of its half length along. The two front wheels steer. `ground_clearance` sets how high the chassis bottom sits above flat ground at rest.

| Field | What it does |
| --- | --- |
| `drive` | which wheels the engine drives: `FrontWheel`, `RearWheel` or `AllWheel` |
| `suspension_min`, `suspension_max`, `suspension_frequency`, `suspension_damping` | the springs: how short and long, how stiff (Hz), how damped |
| `engine_torque`, `max_rpm` | the engine; the gearbox shifts by itself |
| `max_steer_degrees` | how far the front wheels turn at full lock |
| `brake_torque`, `handbrake_torque` | per wheel (the handbrake acts on the rear wheels) |
| `grip` | scales the tyres' friction: 1 is a road tyre, below it slides, above it sticks |

Editing the component while playing rebuilds the vehicle. A `Vehicle` with no collider or on a static body is left alone.

## Driving it

`VehicleInput` is what drives the car (it is added beside the `Vehicle`): `throttle` -1 to 1 (negative reverses), `steer` -1 to 1 (positive is right), `brake` and
`handbrake` 0 to 1. Write it from anywhere, or use the script API. `VehicleState` is what the car is doing, written after every physics step: `speed` (m/s along
the nose), `rpm`, `gear`, `steer_angle`, `wheels_on_ground` and `airborne`.

```lua
Vehicles.Drive(car, 1, 0)                  -- full throttle, straight; optional brake and handbrake arguments follow
print(Vehicles.Speed(car))                 -- m/s along the nose
print(Vehicles.WheelsOnGround(car))        -- 0 to 4
local p = Vehicles.WheelPosition(car, 0)   -- a wheel's world position, to place a wheel mesh
local a = Vehicles.WheelSpin(car, 2)       -- how far it has turned, radians
Vehicles.Flip(car)                         -- stand it back on its wheels where it is
```

## In the player

The player owns the system beside the physics scene and applies it in the physics step, just before the bodies move. Each step it takes apart any vehicle whose
entity is gone, whose component was disabled or edited, then builds new ones, so a constraint never outlives the body it belongs to.
