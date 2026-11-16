# Water and boats

The water kit adds **things that float**: a sea or lake with waves and a current, bodies that bob on it, tip and roll back upright, and boats with a propeller and a
rudder. It is an optional part of the build (`AETHER_KIT_WATER`, on by default) and needs the physics module. It does not draw water: the renderer's water material
does that. The kit is the physics side, and a `WaterSurface` is placed to match the water you see.

It is **not** the force-field `Buoyancy` mode. That one lifts a body by how much of its bounding box is below a flat top, with nothing to tip it. This kit lifts it
column by column, at points, so a body pitches and rolls with the waves and rights itself.

## The components

**`WaterSurface`** is a body of water. Its entity's position is the still-water level (its Y) and, for a bounded one, the middle. The surface is two travelling waves
over the level (the second shorter, lower and turned 37 degrees, so it is not a row of identical ridges), and a current moves what floats in it.

| Field | What it does |
| --- | --- |
| `infinite`, `half_extents` | covers the whole plane, or only the box of `half_extents` (X and Z) round the entity. A bounded surface wins over an infinite one where they overlap |
| `density` | kg/m^3: fresh water 1000, sea 1025 |
| `wave_height`, `wave_length`, `wave_speed`, `wave_direction` | the first wave's crest, crest to crest, how fast crests travel, and the heading in degrees (0 is +X, 90 is +Z). Height 0 is flat water |
| `current_speed`, `current_direction` | the water's own motion, m/s and degrees |

**`Floater`** goes on a body that has a dynamic `RigidBody` and a collider. The hull is a box of `half_extents` round `offset` (in the body's space), cut into
`columns_x` by `columns_z` columns. Each column gets the upward force of the water it pushes aside, **at its own point**, plus drag from the water, so the body settles
where its weight meets the water it displaces (a 400 kg raft on a 2 m by 2 m hull sinks 0.1 m), rocks on the swell, and rolls back when tipped.

| Field | What it does |
| --- | --- |
| `half_extents`, `offset` | the hull, m |
| `columns_x`, `columns_z` | 1 to 12 each: more is smoother and costs more |
| `buoyancy_scale` | 1 is the water's true lift; more floats it higher |
| `linear_drag`, `angular_drag` | per second where the hull is wet. Bobbing is damped at the full rate, gliding at a quarter of it, so a boat can move but a raft does not ring |

**`Boat`** goes on an entity that already has a `Floater`. A propeller at `propeller_offset` pushes it along its **+Z** axis (`thrust`, N at full throttle), but only
while that point is under the water (it ramps up over the first 0.4 m of depth). A rudder turns it by `turn_torque`, more the nearer its speed is to `turn_speed`, so a
boat that is not moving does not turn. `keel_drag` resists sliding sideways, `reverse_ratio` is how strong reverse is, and `max_speed` is where the propeller stops
pushing.

## Scripts

```lua
local height = Water.HeightAt(x, z)        -- the surface there, waves included; a huge negative number where there is no water
if Water.IsWater(x, z) then ... end
local current = Water.Current(x, z)         -- a vector
local under = Water.Submersion(entity)      -- 0 to 1, from the last step
Water.Drive(boat, throttle, steer)          -- throttle -1 to 1, steer -1 (left) to 1 (right); kept until changed
```

`Water.Drive` returns false for an entity that is not an enabled `Boat`, or a number that is not finite. Steering right is a clockwise turn seen from above.

## Notes

- The waves are a function of the water system's own clock, advanced by each physics step, so a run repeats exactly. `WaterSystem::SetTime` sets it.
- Waves and current can be changed while playing by editing the `WaterSurface` (a script can set its fields), for example to raise the sea with a storm.
- The kit pushes bodies; it does not make splashes or sounds. A script can watch `Water.Submersion` for the moment a body goes in.
