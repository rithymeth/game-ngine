# Quests

The quests kit tracks what a player has been asked to do: a list of objectives per quest,
quests that unlock other quests, and rewards. It is an optional part of the build
(`AETHER_KIT_QUESTS`, on by default) and doesn't depend on the other kits. The build spec is
[PHASE_SPECS §30.10](../design/PHASE_SPECS.md).

## A quest

A quest is a `.aquest` file under your content folder; it is cooked automatically.

```json
{
  "name": "Pelts",
  "title": "Wolf Pelts",
  "title_key": "quest.pelts.title",
  "description": "The tanner needs three pelts.",
  "objectives": [
    { "id": "collect", "kind": "count", "target": "Wolf Pelt", "required": 3, "text": "Collect wolf pelts" },
    { "id": "report", "kind": "flag", "target": "tanner", "text": "Bring them to the tanner" },
    { "id": "rare", "kind": "count", "target": "Rare Pelt", "optional": true }
  ],
  "prerequisites": ["Intro"],
  "rewards": { "effects": ["Blessing"], "items": [{ "item": "Gold", "count": 50 }] }
}
```

- An objective has an `id`, a `kind` (`count`, `tag` or `flag`), a `target` and how many are `required` (default 1). **Optional** objectives don't have to be done for the quest to complete.
- `prerequisites` are quests that must be completed first.
- `rewards.effects` are gameplay effects applied to the player on completion (a heal, a buff). `rewards.items` are given to the player's inventory when the inventory kit is built.
- `title_key`, `description_key` and each objective's `text_key` are localization keys; the plain text is the fallback.

A broken quest, a prerequisite that isn't a quest, a cycle of prerequisites, or a reward effect that
doesn't exist is a warning in the player (it names the file or the quest).

## Progress

Each entity has a quest log, saved with it. A quest is *inactive*, *active*, *completed* or
*failed*; a completed quest can't be started again, an abandoned or failed one starts over.

- `Quests.Start (owner, quest)` starts a quest if its prerequisites are completed.
- `Quests.Notify (owner, kind, target, amount)` tells the log something happened: it advances every unfinished objective of the owner's active quests with that kind and target. This is what your game calls ("count", "Wolf Pelt", 1 when a wolf dies; "flag", "tanner" when you talk to the tanner).
- `Quests.Progress (owner, quest, objective, amount)` advances one objective by name.
- Count and tag objectives stop at `required`; a negative amount takes progress back until the objective is done; a flag is done at once.
- When every non-optional objective is done the quest completes and its rewards are paid.

Items are reported for you: when the inventory kit is built, picking up an item notifies
`count` objectives that name it, and dropping it takes the progress back.

## Events

An entity's script hears `OnQuestStarted (quest)`, `OnQuestProgress (quest, objective, progress,
required)`, `OnQuestObjectiveCompleted (quest, objective)`, `OnQuestCompleted (quest)`,
`OnQuestFailed (quest)` and `OnQuestAbandoned (quest)`. A Blueprint has `Event.OnQuestStarted`,
`Event.OnQuestProgress`, `Event.OnQuestCompleted` and `Event.OnQuestFailed`.

```lua
local Hero = {}

function Hero:OnStart()
    Quests.Start(self.entity, "Pelts")
end

function Hero:OnQuestProgress(quest, objective, progress, required)
    ShowMessage(Quests.GetTitle(quest) .. ": " .. progress .. "/" .. required)
end

function Hero:OnQuestCompleted(quest)
    ShowMessage("Quest complete: " .. Quests.GetTitle(quest))
end

return Hero
```

Also in Luau: `Abandon`, `Fail`, `IsActive`, `IsCompleted`, `GetState` ("inactive", "active",
"completed", "failed") and `GetProgress`. The Blueprint `Quests` library has the same (`StartQuest`,
`NotifyQuests`, `ProgressQuest`, `GetQuestState`, and so on).

## Not built yet

A quest journal panel in the editor, timed quests, and objectives that watch the game on their own
(a tag appearing, a place reached): for now your scripts call `Notify`.
