# Aether Engine -- unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged, so it describes what is on `master`, not a promise of a version number or a date.

## Crafting

A new optional kit and engine plugin, **Crafting** (`AETHER_KIT_CRAFTING`, needs Inventory): `.arecipe` files list inputs and outputs, and `CraftingSystem::Craft` carries them out on an inventory all or nothing -- if the outputs do not fit, the inputs are given back. A Luau `Crafting` table (`Craft`, `CanCraft`, `MissingCount`, `Exists`, `GetTitle`, `Describe`) exposes it; see `docs/manual/16-crafting.md`.

## Time of day

A new optional kit and engine plugin, **Time of Day** (`AETHER_KIT_TIMEOFDAY`, no dependencies, no asset files): the game's world clock. An hour and a day count advance with each frame (a game day is 20 real minutes by default, scalable or pausable), with a phase of the day (dawn, day, dusk, night) and a sunlight level from 0 to 1 that scripts can use to light a scene. The clock belongs to the game, so it runs on through a scene change. A Luau `TimeOfDay` table exposes it; see `docs/manual/18-time-of-day.md`.

## Dialogue

A new optional kit and engine plugin, **Dialogue** (`AETHER_KIT_DIALOGUE`, no dependencies): `.adialogue` files describe a conversation as nodes with a next node or choices, `DialogueSystem` runs one per listening entity (`Start`, `Advance`, `Choose`, `End`), and each line can carry a free `event` word a script reacts to. A Luau `Dialogue` table exposes it; see `docs/manual/17-dialogue.md`.

## More of the engine as plugins

Ten more optional modules are now plugins with an `.aplugin` descriptor and a runtime module that registers their components: **Animation**, **UI**, **VFX**, **Sprite2D**, **Sequencer**, **Streaming**, **Gameplay**, and the genre kits **Inventory**, **Interaction** and **Quests** (each kit depends on Gameplay, which starts first). Like Physics, Audio, Navigation, AI and Networking they are enabled by default and built when their module is, so a project can turn one off in the Plugins panel. A new engine plugin is a descriptor, a short module and one name in `plugins/CMakeLists.txt`.

## Packs survive a scene change

An `Inventory` now follows its entity through a scene change the same way a quest log does: the next scene's entity with the same GUID (usually the player) keeps its items, capacity and equipment. An entity with no GUID is left behind, and an entity of the new scene with no counterpart keeps the inventory its own scene gave it. Before this a scene change emptied the player's pack, so a game with the Inventory (or Crafting) kit across several scenes had to keep its items elsewhere.

## Quests survive a scene change

A quest log (`QuestLog`) now follows its entity through a scene change (`Scene.Load`, a door, a respawn): the next scene's entity with the same GUID, usually the player, gets the log of the one being left, with every objective's progress. An entity with no GUID, or with no counterpart in the next scene, is not carried. Before this a scene change started the player's quests over, because the log lives on the entity and the world was replaced.

## Blueprint and Renderer plugins

**Blueprint** (visual scripting: `BlueprintInstance`) and **Renderer** (lights, sky light, post-process volumes and the other render components) are now plugins too, enabled by default like the rest, so every engine module that registers components of its own is a plugin. The modules that register none (Terrain, Loc, Save, Scripting, Templates) are not.
