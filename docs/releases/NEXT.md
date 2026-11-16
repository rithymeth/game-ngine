# Aether Engine -- unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged, so it describes what is on `master`, not a promise of a version number or a date.

## Automatic exposure

Post-process volumes gain **auto exposure** (`auto_exposure`, 0 to 1, off by default). It adapts exposure to the HDR scene's brightness; the existing exposure compensation still biases the result. The setting blends across volumes, and old scenes retain manual exposure.

The GPU meters a grid of at most 32 by 32 samples before bloom, fog and depth of field, reduces their log luminance, and adapts EV100 in two alternating one-pixel textures. Black and non-finite samples are ignored, luminance is bounded to -10 to 10 stops, and the result is limited to -4 to 16 EV100. Brightening scenes adapt at 3/s and darkening scenes at 1/s. Camera switches, scene-name changes, time rewinds, resizes and skipped or disabled metering reset adaptation; the first valid frame uses its own brightness immediately. Disabling it releases the metering textures. This is coarse geometric-mean metering, not percentile histogram metering or local exposure, and small features between grid samples can be missed.

The exposure multiplier stays on the GPU, with no readback or CPU wait, and is applied once before tone mapping, after depth of field. Its texture index shares a word with bloom, keeping the post constants at 128 bytes. D3D12 and Vulkan tests compare the output against the CPU exposure math, including adaptation, volume blending and serialization, black pixels, odd and one-pixel viewports, reset/fallback behavior and composition with bloom and depth of field.

## Cinematic depth of field

The active perspective camera's `CineCamera` now drives depth of field from its focal length, sensor height, aperture and focus distance. Lower f-stops widen the blur; animating `focus_distance` racks focus. Ordinary cameras and non-HDR rendering remain unchanged. Invalid lens parameters or unavailable depth/render targets leave the image sharp.

A thin-lens circle of confusion controls a depth-aware 32-tap disk blur, capped at a 12-pixel radius. It runs on an HDR composite after reflections, occlusion, fog and bloom, before exposure, tone mapping, vignette and film grain. The two intermediate textures resize with the viewport and are allocated only when needed; the original 128-byte post constants remain unchanged. This is a bounded screen-space approximation: transparent surfaces use the opaque depth behind them, hidden background cannot be recovered, and foreground bokeh is not scattered across sharp silhouettes.

CPU tests cover lens math and invalid inputs. D3D12 and Vulkan tests check sharp focus, near/far blur, rack focus, frame-state reset, viewport resizing and composition with other post effects.

## Screen-space reflections

Smooth surfaces now reflect the scene around them, not only the water: the post-process volume gains **screen-space reflections** (0 to 1, default 0.5; 0 is off, `AETHER_RENDER_SSR=0` turns them off for a comparison, and orthographic cameras and pre-HDR devices keep the old look). The forward renderer has no G-buffer, so the scene shader writes each opaque surface's roughness into the HDR target's alpha (the mirror pass and blended draws leave it alone: the water already mirrors the scene, and a blend's alpha is the blend's), and the post pass reconstructs the view-space position and normal from the depth copy it was already taking for occlusion and the fog. It reflects the eye ray about that normal, marches it against the depth in view space with growing steps, refines the crossing by binary search, and mixes the colour it lands on in by Fresnel and the frame's edge fade. A rough surface (above 0.55), a miss (the ray leaves the screen or passes a silhouette), or no depth at all fades back to the sky lighting the surface already had, so no reflection probe is needed; a miss above the horizon therefore reflects the sky's image-based light rather than an SSR-less void. The strength rides in the 8 bits of the tone mapper's word beside the tone mapper, so `PostConstants` stays 128 bytes, and temporal anti-aliasing now passes the alpha through unfiltered (it is a surface property, not colour). The GPU test builds a floor-and-wall scene analytically, works out where the floor's mirror ray meets the wall, and checks the pass's output against that colour on D3D12 and Vulkan.

## Force fields

A new optional kit and engine plugin, **Force Fields** (`AETHER_KIT_FORCEFIELDS`, needs physics): a `ForceField` component makes a sphere or box volume that pushes the dynamic bodies inside it, every physics step, in one of five modes: **directional** (wind, a conveyor, a lift), **radial** (a gravity well or repeller, with a falloff), **vortex** (a whirlpool or tornado around the field's up axis), **buoyancy** (water: a body is held up by how much of it is below the surface, with optional fluid drag) and **drag** (mud, thick air). A push can be an acceleration (a heavy and a light body move alike) or a force. A Luau `Forces.Explode(position, radius, strength [, upward])` gives every dynamic body near a point a change of velocity away from it, fading to nothing at the radius. The player applies the fields inside its physics step, just before the bodies move; the component is available in the editor and through the MCP like the other kits'. See `docs/manual/23-force-fields.md`.

## Destruction

A new optional kit and engine plugin, **Destruction** (`AETHER_KIT_DESTRUCTION`, needs physics): a `Destructible` component on an entity with a box collider shatters it into a grid of physics pieces (each with a share of the mass and the source's model scaled down) when something hits it hard enough (`break_speed`), when damage uses up its `health`, or on demand. Pieces are `Debris`: they expire after `piece_lifetime` and the oldest go first beyond a cap of 400. Luau gets `Destruction.Damage`, `Destruction.Shatter` and `Destruction.Debris`. See the manual's Destruction chapter.

The player now installs its physics event handler once the physics scene exists. It was installed while the runtime started, before the scene was made, so scripts did not receive `OnCollisionBegin` and the other physics events in the first scene.

## Vehicles

A new optional kit and engine plugin, **Vehicles** (`AETHER_KIT_VEHICLES`, needs physics), on Jolt's vehicle constraint: a `Vehicle` component on a dynamic body with a box collider gives it four wheels on springs (front wheels steer), an engine with an automatic gearbox, brakes and a handbrake, and front, rear or all-wheel drive. A `VehicleInput` component (or Luau `Vehicles.Drive`) drives it, `VehicleState` reports speed, rpm, gear and wheel contact, and `Vehicles.Flip` rights a rolled car. Luau gets the `Vehicles` table (`Drive`, `Speed`, `WheelsOnGround`, `WheelPosition`, `WheelSpin`, `Flip`). See the manual's Vehicles chapter.

## Virtual geometry, stage 1

The first stage of a Nanite-style renderer: `BuildClusterHierarchy` (`aether/cook/cluster_hierarchy.h`) builds a DAG of meshlet clusters offline, each level about half the triangles of the one below, with group borders locked so the pieces always join. `SelectClusters` is the CPU reference for the cut: a cluster is drawn when its own error looks small enough on screen and its parent's does not, and the result is watertight at every distance. Nothing is cooked or drawn yet, so no format changes. See `docs/design/VIRTUAL_GEOMETRY.md` for the plan, the measurements and the known limits.

## Virtual geometry, stage 2

The cluster cut and the culling now run on the GPU: `ClusterCullPass` (`player/src/cluster_cull_pass.h`) is a compute pass that picks the same clusters as the CPU selector (checked over 112 distance, threshold and culling-mode combinations, with no differences), with frustum and normal-cone back-face culling. `cook::SelectClusters` gained `cull_backfaces`. The RHI gained two optional calls, `ICommandList::HostReadBarrier` and `IDevice::ReadStorageBuffer` (with `SupportsStorageBufferRead`), implemented on Vulkan only. Not drawn yet and not benchmarked on a real GPU. See `docs/design/VIRTUAL_GEOMETRY.md`.

## Loot

A new optional kit and engine plugin, **Loot** (`AETHER_KIT_LOOT`, needs Inventory): `.aloot` files describe a loot table as weighted entries (each with a count range, and an empty item for "nothing") and a number of rolls, and `LootSystem` rolls one and gives the drops to an entity's inventory. Rolls are deterministic (the same table and seed always drop the same, and the kit's own seed stream starts the same every run unless a script seeds it); loot is not all or nothing, so what does not fit is lost and `LastGiven` says what went in. A Luau `Loot` table (`Give`, `LastGiven`, `Roll`, `SetSeed`, `Exists`, `GetTitle`) exposes it; see `docs/manual/22-loot.md`.

## Checkpoints

A new optional kit and engine plugin, **Checkpoints** (`AETHER_KIT_CHECKPOINTS`, no dependencies): a `Checkpoint` component marks a place, and when an entity carrying its `activator_tag` (the player, by default) comes within its `radius` it becomes the last checkpoint. The last one, its scene and which have been reached are kept in the game's save bag, so they are saved and loaded with the slot and a scene change does not touch them. The checkpoint's script hears `OnCheckpoint`, and a Luau `Checkpoints` table (`Last`, `GetScene`, `IsReached`, `Exists`, `Activate`, `Respawn`) puts an entity back at the last one. The component is available in the editor and through the MCP like the other kits'. See `docs/manual/21-checkpoints.md`.

## Spawner

A new optional kit and engine plugin, **Spawner** (`AETHER_KIT_SPAWNER`, no dependencies): a `Spawner` component on an entity makes instances of a package prefab appear around it, `burst` at a time every `interval` seconds, while fewer than `max_alive` of its spawns are alive and until `total` have been made, scattered within `radius` by a deterministic function so a run is the same every time. The interval counts only while there is room, the spawner's script hears `OnSpawned` with each new entity, and a Luau `Spawner` table (`SpawnNow`, `AliveCount`, `TotalSpawned`) lets a script call a wave at once. The component is available in the editor and through the MCP like the other kits'. See `docs/manual/20-spawner.md`.

## Achievements

A new optional kit and engine plugin, **Achievements** (`AETHER_KIT_ACHIEVEMENTS`, no dependencies): `.aachievement` files describe goals with a name, a title and a target count, and `AchievementSystem` counts progress toward them (`Add`, `Unlock`, `Reset`), unlocking the moment a target is reached and queuing each unlock once for the game's toast (`NextUnlocked`). Progress lives in the game's save bag, as an integer under `achievement.<name>`, so it is saved and loaded with the slot and a scene change does not touch it. A Luau `Achievements` table exposes it; see `docs/manual/19-achievements.md`.

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

## Ropes

A new optional kit and engine plugin, **Ropes** (`AETHER_KIT_ROPES`, needs physics), on the physics joints: a `Rope` component becomes a chain of small bodies joined by ball-and-socket joints, hanging from the world or from another body, optionally carrying a body at its free end, with an optional model drawn per segment. Luau gets the `Ropes` table (`Cut`, `Length`, `Segments`, `SegmentPosition`). A cut wakes the rope's bodies: one that had hung at rest would otherwise stay in the air. See the manual's Ropes chapter.

## Weather

A new optional kit and engine plugin, **Weather** (`AETHER_KIT_WEATHER`): the sky's mood as numbers (clouds, rain, snow, fog, wind with gusts, lightning with a thunder delay, wet ground) that blend smoothly from one kind of weather to the next, by hand (`Weather.Set`) or on their own from a seed, the same at any frame rate. It is the game's, like the clock, so it keeps going through a scene change. It adds a frame stage, `Player.Weather`, after `Player.Update` (the stage-order snapshot test now lists it). The kit only keeps the numbers; what a scene does with them is the game's. Luau gets the `Weather` table. See the manual's Weather chapter.
