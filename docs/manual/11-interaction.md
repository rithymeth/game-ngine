# Interaction

The interaction kit is for everything a character walks up to and uses: doors, levers,
chests, people to talk to, pickups. It is an optional part of the build
(`AETHER_KIT_INTERACTION`, on by default) and works with the [gameplay](09-gameplay.md)
tags, effects and abilities. The build spec is
[PHASE_SPECS §30.8](../design/PHASE_SPECS.md).

## The Interactable component

Put an `Interactable` on the thing to use (and give it a `Transform`):

| Field | What it does |
| --- | --- |
| `enabled` | Off, and nothing can use it. |
| `prompt`, `prompt_key` | The text shown ("Open"), or a localization key for it. |
| `range` | How close the user must be, in the scene's units; 0 for no limit. |
| `required_tags`, `blocked_tags` | The user must have all of the first and none of the second (`Item.Key` also matches `Item.Key.Gold`). |
| `effect` | An effect applied to the user when it is used (a heal, a buff). |
| `ability` | An ability the user activates (the user must have been granted it). |
| `one_shot` | Can be used once; `Reset` makes it usable again. |
| `cooldown` | Seconds before it can be used again. |

What using it *does* is up to your game: the thing's own script or Blueprint hears
`OnInteract` with the entity that used it.

## Finding and using

`Interaction.Find (interactor, x, y, z, fx, fy, fz)` returns the nearest usable thing within
its range and in front of the direction you give (a zero direction looks all round), or
nothing. Show its `GetPrompt`, and on the key press call `Interact (interactor, target)`.
It returns true if it was used; if not, the user's script hears `OnInteractFailed (target,
reason)` with a reason such as `out_of_range`, `missing_tag`, `blocked_tag`, `cooldown`,
`used`, `disabled` or `action_failed`.

```lua
local Hero = {}

function Hero:OnUpdate(dt)
    local p, f = self.position, self.forward            -- from your own movement code
    local target = Interaction.Find(self.entity, p.x, p.y, p.z, f.x, f.y, f.z)
    self.prompt = target and Interaction.GetPrompt(target) or ""
    if target and Input.GetBool("Use") then
        Interaction.Interact(self.entity, target)
    end
end

function Hero:OnInteractFailed(target, reason)
    if reason == "missing_tag" then ShowMessage("It's locked.") end
end

return Hero
```

```lua
local Door = {}

function Door:OnInteract(who)
    self.open = not self.open                   -- animate it, play a sound...
end

return Door
```

## From Blueprints

The `Interaction` library has `FindInteractable`, `GetInteractionPrompt`, `GetPromptFor`,
`CanInteract`, `Interact`, `SetInteractable` and `ResetInteractable`, and the events are
`Event.OnInteract (interactor)` on the thing used and `Event.OnInteractFailed (target, reason)`
on the user.

## Not built yet

Hold-to-use interactions with a progress bar, interaction through a physics ray (positions are
`Transform` positions for now), and prompts drawn by the UI automatically.
