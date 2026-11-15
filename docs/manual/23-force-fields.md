# Force fields

The force-fields kit adds **volumes that push physics bodies**: wind that carries crates along, a gravity well, a whirlpool, water that holds things up, mud that
slows them, and an explosion. A `ForceField` component on an entity is the whole setup. The kit is an optional part of the build (`AETHER_KIT_FORCEFIELDS`, on by
default) and needs the physics module.

## The component

The volume is at the entity's `Transform`: a **sphere** of `radius`, or a **box** of `half_extents` that turns with the entity. A body is inside while its collider
touches the volume; the push is worked out from where its centre of mass is. Only **dynamic** bodies are pushed (static and kinematic ones are left alone), and
the push is added every physics step, just before the bodies move.

| Mode | What it does | `strength` is |
| --- | --- | --- |
| `Directional` | pushes along `direction` (any length): wind, a conveyor, a lift, anti-gravity | the push, in m/s² |
| `Radial` | pulls toward the centre (positive) or pushes away from it (negative): a gravity well, a repeller, a fan | the push, in m/s² |
| `Vortex` | pushes around the field's up axis, which turns with the entity: a whirlpool, a tornado | the push, in m/s² |
| `Buoyancy` | lifts what is below the top of the volume: water | the fraction of gravity cancelled at full submersion (1 holds a body up, 2 floats it up) |
| `Drag` | slows what moves through it: mud, thick air, deep water | the coefficient, per second |

- `falloff` (Radial and Vortex): 0 is the same push everywhere, 1 fades it linearly to nothing at the edge, higher values fade faster.
- `mass_independent` (Directional, Radial and Vortex): on, `strength` is an acceleration, so a heavy and a light body move alike; off, it is a force in newtons and a heavy body moves less. Buoyancy and Drag are always accelerations.
- Buoyancy works out how much of a body is under the surface (the top of the box, or of the sphere) from the body's bounds, so a body half under the surface is half held up. `fluid_drag` also slows what is in the water, per second at full submersion.
- A body can be in several fields at once; their pushes add up.

Everything is an ordinary reflected component: a script switches a field on and off, changes its strength, or turns a wind round with `entity:Get("ForceField")`.

## Explosions

`Forces.Explode(position, radius, strength [, upward])` gives every dynamic body whose collider touches the sphere a change of velocity of `strength` m/s away from
`position`, fading linearly to nothing at `radius`, and the same for a heavy and a light body. `upward` (0 to 1) adds a lift: at 1 the push is 45 degrees up. A body
exactly on the centre goes straight up. It returns how many bodies it pushed (0 for a radius that is not positive, or a strength or position that is not a number).

```lua
function Barrel:OnDestroyed()
    local here = self.entity:Get("Transform").position
    local hit = Forces.Explode(here, 8, 12, 0.4)
    print("The blast threw " .. hit .. " bodies")
end
```

Fields are applied, and explosions land, in the physics step, so their effect is deterministic for a given scene: the same fields push the same bodies in the same
order every run.
