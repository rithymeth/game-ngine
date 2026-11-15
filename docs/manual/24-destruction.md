# Destruction

The destruction kit adds **things that break**: a crate that bursts when something hits it hard, a wall that gives way after enough damage, a window you shoot
out from a script. A `Destructible` component on an entity with a `BoxCollider` is the whole setup. The kit is an optional part of the build
(`AETHER_KIT_DESTRUCTION`, on by default) and needs the physics module.

## The component

When it breaks, the entity is replaced by a grid of `pieces_x` x `pieces_y` x `pieces_z` smaller boxes (each axis 1 to 6, so at most 216 pieces). Each piece is a
dynamic body with a share of `mass`, takes the source's `ModelRenderer` scaled down to fit, and starts moving: it is thrown outward from the point of the break at
`burst_speed`, plus `inherit_velocity` of whatever speed the whole thing had. The entity's children are not broken with it.

| Field | What it does |
| --- | --- |
| `break_speed` | breaks when something hits it at least this fast (m/s along the contact); 0 means a hit never breaks it |
| `health` | the damage it takes before breaking; 0 means damage does not break it, only a hard hit does |
| `piece_lifetime` | seconds before each piece is removed; 0 keeps them |

## Debris

The pieces carry a `Debris` component. They are removed when their lifetime runs out, and the oldest go first when more than 400 are lying about.

## From a script

```lua
Destruction.Damage(entity, 25)            -- true when that used up its health; it breaks at the next physics step
Destruction.Shatter(entity, point)        -- break it now, bursting from `point` (optional)
print(Destruction.Debris())               -- how many pieces there are
```

Breaking happens between physics steps, never while the physics events are still being delivered.

## In the player

The player owns the system beside the physics scene, feeds it every physics event and updates it in the physics step. The player now installs its physics
event handler once the physics scene exists, so scripts also receive `OnCollisionBegin` and the other physics events from the first scene on.
