# Aether Engine -- unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged, so it describes what is on `master`, not a promise of a version number or a date.

## Screen-space reflections

Smooth surfaces now reflect the scene around them, not only the water: the post-process volume gains **screen-space reflections** (0 to 1, default 0.5; 0 is off, `AETHER_RENDER_SSR=0` turns them off for a comparison, and orthographic cameras and pre-HDR devices keep the old look). The forward renderer has no G-buffer, so the scene shader writes each opaque surface's roughness into the HDR target's alpha (the mirror pass and blended draws leave it alone: the water already mirrors the scene, and a blend's alpha is the blend's), and the post pass reconstructs the view-space position and normal from the depth copy it was already taking for occlusion and the fog. It reflects the eye ray about that normal, marches it against the depth in view space with growing steps, refines the crossing by binary search, and mixes the colour it lands on in by Fresnel and the frame's edge fade. A rough surface (above 0.55), a miss (the ray leaves the screen or passes a silhouette), or no depth at all fades back to the sky lighting the surface already had, so no reflection probe is needed; a miss above the horizon therefore reflects the sky's image-based light rather than an SSR-less void. The strength rides in the 8 bits of the tone mapper's word beside the tone mapper, so `PostConstants` stays 128 bytes, and temporal anti-aliasing now passes the alpha through unfiltered (it is a surface property, not colour). The GPU test builds a floor-and-wall scene analytically, works out where the floor's mirror ray meets the wall, and checks the pass's output against that colour on D3D12 and Vulkan.

## Lens effects in the post volume

The post-process volume gains three Unreal-style lens effects, each from 0 (off, the default) to 1: **chromatic aberration** pulls the red and blue channels apart toward the edges of the screen, **film grain** adds moving noise to the display image (strongest in the midtones, new every frame, the same on all three channels), and **sharpen** brings edges out by sharpening luminance only, in a compressed domain so a bright highlight can not ring. They blend between volumes like the other settings, and a scene saved before they existed loads with them off. They are in the post pass's single composite, so there is no extra pass. To keep the push constants within the 128 bytes every device guarantees, the vignette and the three new strengths now travel as four 8-bit values in one word, and the tone mapper shares its word with the frame number that moves the grain; `PostConstants` has `lens` where it had `vignette`. The CPU references (`FilmGrainNoise`, `SharpenLuminance`, `ChromaticOffset` in `renderer/post.h`) are what the GPU test compares the pass against, pixel for pixel.

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
