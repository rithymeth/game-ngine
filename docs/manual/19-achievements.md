# Achievements

The achievements kit tracks goals: "walk out of the camp", "take 50 pieces of salvage". It is an optional part of the build
(`AETHER_KIT_ACHIEVEMENTS`, on by default) and needs no other kit. What an unlock *does* (a toast, a reward, a trophy on a platform) is your
game's business: the kit counts, remembers and tells your script what was unlocked.

## An achievement

An achievement is a `.aachievement` file under your content folder; it is cooked automatically.

```json
{
  "name": "Salvager",
  "title": "Salvager",
  "description": "Take 50 pieces of salvage.",
  "target": 50,
  "hidden": false
}
```

- `name` is required and is what scripts use. `title` falls back to the name.
- `target` is how many it takes (1 to 1,000,000; default 1, a one-off goal).
- `hidden` is a flag for your UI: keep it out of the list until it is unlocked (`Achievements.IsHidden`).

A file with no name, a target out of range, or a field of the wrong kind is a warning in the player that names the file.

## Progress

`Add(name, amount)` counts toward the target and stops there; reaching it unlocks the achievement the moment it happens. `Unlock(name)` unlocks at
once, and `Reset(name)` takes the progress away.

Progress lives in the **save bag**, as an integer under `achievement.<name>`. That is the bag a save slot writes, so the progress is saved and
loaded with the slot (`SaveGames.SaveToSlot` / `LoadFromSlot`) and a scene change does not touch it. Two consequences: loading a slot does not
announce old unlocks again, and a new game (`SaveGames.CreateSaveObject()`, which empties the bag) also clears the achievements. A game that wants
them to outlast a new game keeps its own copy of the unlocked names.

## From a script

```lua
-- when the player takes salvage
Achievements.Add("Salvager")

-- every frame, in the HUD script: show a toast for each new unlock
local name = Achievements.NextUnlocked()
while name ~= "" do
    UI.SetText("ToastText", "Achievement: " .. Achievements.GetTitle(name))
    name = Achievements.NextUnlocked()
end
```

| Call | Result |
| --- | --- |
| `Achievements.Add(name [, amount])` | `"ok"`, `"unknown_achievement"` or `"bad_amount"` (1 to 1,000,000) |
| `Achievements.Unlock(name)` / `Reset(name)` | `"ok"` or `"unknown_achievement"` |
| `Achievements.IsUnlocked(name)` / `Exists(name)` / `IsHidden(name)` | a boolean |
| `Achievements.GetProgress(name)` / `GetTarget(name)` | a number (0 for an unknown name) |
| `Achievements.GetTitle(name)` / `GetDescription(name)` | a string |
| `Achievements.UnlockedCount()` | how many are unlocked |
| `Achievements.NextUnlocked()` | the name of one unlocked since you last asked, or `""` when there is none |

The unlock queue belongs to the scene: a toast not shown before a scene change is dropped (the unlock itself stays).
