# Aether Engine -- unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged, so it describes what is on `master`, not a promise of a version number or a date.

## Crafting

A new optional kit and engine plugin, **Crafting** (`AETHER_KIT_CRAFTING`, needs Inventory): `.arecipe` files list inputs and outputs, and `CraftingSystem::Craft` carries them out on an inventory all or nothing -- if the outputs do not fit, the inputs are given back. A Luau `Crafting` table (`Craft`, `CanCraft`, `MissingCount`, `Exists`, `GetTitle`, `Describe`) exposes it; see `docs/manual/16-crafting.md`.

## More of the engine as plugins

Ten more optional modules are now plugins with an `.aplugin` descriptor and a runtime module that registers their components: **Animation**, **UI**, **VFX**, **Sprite2D**, **Sequencer**, **Streaming**, **Gameplay**, and the genre kits **Inventory**, **Interaction** and **Quests** (each kit depends on Gameplay, which starts first). Like Physics, Audio, Navigation, AI and Networking they are enabled by default and built when their module is, so a project can turn one off in the Plugins panel. A new engine plugin is a descriptor, a short module and one name in `plugins/CMakeLists.txt`.

## Quests survive a scene change

A quest log (`QuestLog`) now follows its entity through a scene change (`Scene.Load`, a door, a respawn): the next scene's entity with the same GUID, usually the player, gets the log of the one being left, with every objective's progress. An entity with no GUID, or with no counterpart in the next scene, is not carried. Before this a scene change started the player's quests over, because the log lives on the entity and the world was replaced.

## Blueprint and Renderer plugins

**Blueprint** (visual scripting: `BlueprintInstance`) and **Renderer** (lights, sky light, post-process volumes and the other render components) are now plugins too, enabled by default like the rest, so every engine module that registers components of its own is a plugin. The modules that register none (Terrain, Loc, Save, Scripting, Templates) are not.
