# Gameplay: tags, attributes, effects and abilities

The gameplay module gives characters stats, status effects and abilities
without writing the bookkeeping yourself. It works from Blueprints and Luau, and
the player runs it each frame. The build spec is
[PHASE_SPECS §30](../design/PHASE_SPECS.md).

## The pieces

- **Gameplay tags** are dotted names such as `State.Stunned` or `Damage.Fire.Burning`. A check for `Damage.Fire` also matches `Damage.Fire.Burning`. An entity's `TagContainer` counts how many times each tag was added, so two things that both stun a character keep it stunned until both let go.
- **Attributes** are an entity's stats (Health, Mana, Stamina). Each has a *base* (what the game sets and saves), a *current* value (the base after effects, kept within its min and max) and the bounds. Changing one queues `OnAttributeChanged`.
- **Effects** (`.aeffect` files) change attributes: instantly, for some seconds, or until removed. They can add, multiply or override, stack, tick every so often (poison), require or be blocked by tags, and grant tags while they last.
- **Abilities** (`.aability` files) are things an entity can do. They need and are blocked by tags, cost an effect, go on cooldown through another effect, and can cancel or block other abilities.

## A sample

`Content/Effects/Poison.aeffect`, five damage a second for three seconds:

```json
{
  "name": "Poison",
  "duration_policy": "timed",
  "duration": 3,
  "period": 1,
  "modifiers": [{ "attribute": "Health", "op": "add", "magnitude": -5 }],
  "granted_tags": ["State.Poisoned"]
}
```

`Content/Effects/FireballCost.aeffect` and `FireballCooldown.aeffect`:

```json
{ "name": "FireballCost", "modifiers": [{ "attribute": "Mana", "op": "add", "magnitude": -20 }] }
{ "name": "FireballCooldown", "duration_policy": "timed", "duration": 3, "granted_tags": ["Cooldown.Fireball"] }
```

`Content/Abilities/Fireball.aability`:

```json
{
  "name": "Fireball",
  "tags": ["Ability.Fire"],
  "activation_blocked": { "op": "any", "tags": ["State.Stunned"] },
  "cost": "FireballCost",
  "cooldown": "FireballCooldown",
  "max_duration": 1.0
}
```

A Luau script on the character:

```lua
local Hero = {}

function Hero:OnStart()
    Attributes.Define(self.entity, "Health", 100, 0, 100)
    Attributes.Define(self.entity, "Mana", 50, 0, 100)
    Abilities.Grant(self.entity, "Fireball")
end

function Hero:OnUpdate(dt)
    if Input.GetBool("Fire") then
        local handle = Abilities.TryActivate(self.entity, "Fireball")   -- 0 if it can't
    end
end

function Hero:OnAbilityActivated(ability, handle)
    -- spawn the projectile here, then:
    Abilities.End(handle)
end

function Hero:OnAbilityFailed(ability, reason)   -- "cooldown", "cannot_afford", "blocked", ...
end

function Hero:OnAttributeChanged(name, old, new)
    if name == "Health" and new <= 0 then Effects.RemoveByTag(self.entity, "State") end
end

return Hero
```

Poison is applied with `Effects.Apply(target, "Poison", source)`. Mana and Health
change as the effects tick, and the script hears each change.

## From Luau

| Table | Functions |
| --- | --- |
| `GameplayTags` | `IsValid`, `Matches`, `MatchesExact`, `GetParent`, `GetDepth` |
| `Attributes` | `Get`, `GetBase`, `GetMin`, `GetMax`, `Has`, `Define`, `SetBase`, `AddBase` |
| `Effects` | `Apply`, `Remove`, `RemoveByTag`, `HasActive`, `GetActiveCount` |
| `Abilities` | `Grant`, `Revoke`, `TryActivate`, `CanActivate`, `Commit`, `End`, `Cancel`, `IsActive`, `IsGranted` |

Effect and ability handles are numbers; 0 means none. An entity's script defines
`OnAttributeChanged (name, old, new)`, `OnEffectApplied` and `OnEffectRemoved
(effect, handle)`, `OnAbilityActivated (ability, handle)`, `OnAbilityEnded
(ability, handle, cancelled)` and `OnAbilityFailed (ability, reason)` to hear
what happens to it.

## From Blueprints

The same functions are the `GameplayTags`, `Attributes`, `Effects` and
`Abilities` libraries, and the events are `Event.OnAttributeChanged`,
`Event.OnEffectApplied`, `Event.OnEffectRemoved`, `Event.OnAbilityActivated`,
`Event.OnAbilityEnded` and `Event.OnAbilityFailed`. An ability's logic is your
graph: do the work when it activates (the latent nodes such as Delay work here)
and call `EndAbility`.

## Debugging in the editor

The **Gameplay Debugger** (Debug tools) shows every entity that has gameplay data: its
attributes (base, current, bounds, and the modifiers an effect is putting on them), its active
effects with time left and stacks, its abilities (ready or running) and its tags with counts.
Type in the filter to narrow it to an entity, attribute, effect, ability or tag. Type a number
into an attribute's base to set it, press Remove on an effect or Cancel on a running ability,
or add and remove tags. Edits made while the game is running go through the running systems, so
scripts and Blueprints hear the changes.

## Good to know

- A broken `.aeffect` or `.aability` file is a warning in the player (it names the file and the error), not a crash. An ability that names a missing effect is one of those warnings.
- Effects and abilities are cooked automatically; you don't list them.
- Applying an effect to an entity that lacks one of its attributes is rejected as a whole.
- Not built yet: waiting for a gameplay event inside an ability, network replication, and graphs of values over time in the debugger.
