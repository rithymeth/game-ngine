# Fire

The fire kit adds **things that burn**: a crate that catches from a burning neighbour, burns on its fuel while its flames heat what is near, is put out by rain, and is
left charred or burns away. A `Flammable` component on an entity is the whole setup. The kit is an optional part of the build (`AETHER_KIT_FIRE`, on by default) and needs
the VFX and Renderer modules for the flames. It does not use physics.

## The component

| Field | What it does |
| --- | --- |
| `fuel` | seconds it burns for once alight |
| `ignition_heat` | how much heat it takes to catch |
| `spread_radius`, `heat_rate` | while it burns it gives heat to everything within `spread_radius`: `heat_rate` per second right beside it, falling to nothing at the edge |
| `cool_rate` | heat lost per second while nothing is heating it, so a brief scorch fades |
| `wetness`, `douse_wetness` | 0 to 1. Wetness scales heating down; at `douse_wetness` a burning one goes out and a dry-looking one cannot catch at all |
| `consume` | destroy the entity when it has burnt out; off leaves it where it is, burnt (it cannot catch again) |
| `flame_effect`, `flame_offset` | a particle system asset (`.avfx`) drawn while it burns, and where it sits from the entity's position. Empty: no flames drawn |
| `light_color`, `light_intensity`, `light_range` | the flames' point light; intensity 0 for none |

The flames are a **child entity** with a `ParticleSystem` and a `PointLight` that follows the burning thing. When it goes out the particles are told to stop, the light
goes, and the entity is removed three seconds later so the last sparks die away.

## How a fire goes

Each frame, in the player's `Player.Fire` stage (with the frame's update, before scripts run): burning things lose fuel, wet ones go out, every burning thing's heat is
worked out and handed to what is near (all of it first, then taken, so the order of the entities makes no difference), and anything that has taken `ignition_heat` catches.
A thing nearer a fire catches sooner than one further off, and several burning neighbours add up. The same scene gives the same fire every run.

## Scripts

```lua
Fire.Ignite(entity)                -- true if it caught (not already burning, not burnt out, not too wet)
Fire.IgniteNear(x, y, z, radius)   -- every dry Flammable within the radius; returns how many caught: a fire bomb, a lightning strike
Fire.Extinguish(entity)
Fire.IsBurning(entity); Fire.IsBurnt(entity); Fire.Heat(entity); Fire.FuelLeft(entity); Fire.Burning()
Fire.SetWetness(entity, w); Fire.WetAll(w)   -- 0 dry to 1 soaked: feed it the weather's rain
```

With the Weather kit, one line in a script closes the loop: `Fire.WetAll(Weather.Wetness())`, once a frame, and a storm puts the fires out.

## Notes

- A put-out fire keeps the fuel it had left, so a thing can be lit again (once it is dry).
- `Flammable` needs a `Transform`; one without is ignored.
- The kit does not hurt anything. A script that wants damage can poll `Fire.IsBurning` and apply it.
