# Abyss Protocol

**The deeper you descend, the less you can trust what you hear.**

Underwater exploration / survival mystery for Aether Engine. Stylized 3D, single-player first.

## Concept pillars

1. **The ocean listens** — Sound is both a tool and a threat. Active sonar reveals the world but attracts predators.
2. **Darkness is a resource** — Lights help you see, and they also give you away.
3. **Depth changes the rules** — Deeper water drains the battery faster.

## Current prototype

- Acoustic pulses, a sound-sensitive creature, battery and oxygen.
- Win32 software rasterizer (no GPU), same approach as Gem Hop.
- The boat, creatures, rocks, Haven, the relay, and the cache are glTF meshes in [assets/](assets). Positions are 16-bit (`KHR_mesh_quantization`); the node scale restores the size. The boat is `hull.glb` unless you pass another file or set `AETHER_ABYSS_MODEL`. This view uses base color only. A mesh over 4000 triangles keeps its largest faces, so the silhouette stays, instead of dropping every Nth triangle. A restored relay pulses green, and the cache goes dark once taken. The loader also decodes `EXT_meshopt_compression` if a model uses it.
- No model found: an orange placeholder hull is drawn instead.
- **Run**: leave Haven, restore the silent relay, then get home. `F` fires one sonar ping and spends battery. For a few seconds you can see farther and a bright marker points at the objective. After it fades, a dim marker keeps the last heading. It does not update until you ping again, and it clears when the relay answers. Lights show what is nearby, spend battery, and give you away. Two creatures listen. Rocks are solid even when you cannot see them.
- A rock hit also opens a leak. Oxygen falls faster until you reach the cache, which seals it, patches some hull, and refills battery. The creatures patrol, and when they wake they swim to the last sound they heard, not to you. Full thrust leaves a fading wake they can follow. Creep with Shift leaves none. A ping only wakes a creature inside the pulse.
- Once the relay is restored it keeps calling, and the boat carries that signal home for 55 seconds. The title counts it down. The gold mark flashes faster as it runs out, and the run fails if it hits zero before you dock. Creeping is no longer silent: a creature within about 16 metres hears the hull. Haven only cares about a creature near the dock. Lights off, and the dock clear. Deeper than 26 metres the title says `PRESSURE` and the battery drains.

| Key | Action |
| --- | --- |
| W / A / S / D | Thrust, relative to the camera |
| Shift | Quiet thrust. Full speed carries through the water |
| Q / E | Dive / rise |
| Mouse or arrows | Look |
| F | Sonar ping (reveals the bearing, the creature hears it) |
| L | Toggle lights (the view goes dark) |
| R | Restart |
| Esc | Quit |

The window title shows depth, battery, lights, sonar contact time, creature awareness, and the objective. HUD bars are battery (gold), oxygen (blue), and awareness. The short cyan bar on the right is how loud the boat is. The boat and the creatures turn to face the way they are moving. A cyan mark means a ping is still showing you the water.

## Build

The sample is already registered in the root `CMakeLists.txt`.
```powershell
cmake --build build --config RelWithDebInfo --target abyss_protocol
.\abyss_protocol.exe path\to\submarine.glb
```

`AETHER_ABYSS_MODEL` is used when no path is passed. `AETHER_ABYSS_MAX_FRAMES` auto-quits. `AETHER_ABYSS_SCREENSHOT` writes the last frame as a BMP.

Textures are not sampled in this view (flat base color only). The engine's `gltf_demo` is the D3D12 path with real texture sampling and skinning.

## Assets

Prefer glTF 2.0 / GLB, PBR metallic-roughness, moderate triangle counts.

- Sketchfab: stylized submersible, bioluminescent coral
- Meshy.ai and CGTrader free packs: underwater props
- Hugging Face deep-sea fish GLBs for creatures
- Poly Haven for PBR textures when the GPU path is wired

Check the license before shipping. Prefer CC0.

## Next

1. Play the run: quiet approach, then home.
2. Move mesh drawing onto the D3D12 path (`gltf_demo`) so textures and skinning show up.
3. Turn acoustics into a job-friendly ECS system.

## The Haven scene: all 128 models in one Aether scene

The 128 Abyss Protocol models (Meshy exports, AP-001 to AP-128: submarines, the colony's buildings, 43 walkable interiors, creatures, flora, ruins, equipment) are too
heavy and too textured for the software view above, so they are assembled in a scene for the engine's own player:

```
python samples/abyss_protocol/tools/build_haven.py --models <folder with the GLBs> --work <scratch folder> --size 1280x720    # the scene and a 48 s flythrough
python samples/abyss_protocol/tools/build_haven.py --models <folder> --work <scratch> --catalog --size 2560x1440            # one picture of all 128
```

`--stills` renders one frame from each camera stop, `--no-render` only builds and cooks the scene. The models are not in the repository (about 840 MB, and the download
links expire); the builder needs a Vulkan player build, `ffmpeg`, Pillow and, on Linux, lavapipe and `xvfb`.

- **Sizes.** Every model arrives about 1 unit across. `tools/build_haven.py` gives each a real size (`SIZE`, the longest side in metres; the Manta's three hull sections are
  fitted by their cross-section so they meet end to end) and stands it on y = 0.
- **Textures.** The renderer has one table of 128 texture slots for everything it samples, and the models carry 384 images (three 2048 x 2048 maps each), so most would draw
  plain. `tools/haven_models.py` gives the 14 `HEROES` their own textures (cut to 1024) and puts every other model's base colour in two shared 4096 x 4096 atlases of
  512-pixel tiles (the model's UVs move into its tile; the normal and metallic-roughness maps are given up). That is about 50 slots in all, and a run logs no
  "table is full" error. The models shrink from 6.5 MB to about 0.4 MB each.
- **Layout.** The colony (a hub, a ring of buildings, an outer ring of plant), a dock (the drydock gate, the submarines moored in a row, the Manta in three pieces, the
  carrier), a gallery of the 43 walkable rooms, an equipment yard, the Chorus ruins, a wilderness of flora and geology and the creatures (on the seabed and swimming above
  it). Doorways between rooms are **not** connected: nobody has checked that these rooms join up (the kit that made them says so), so each stands alone.
- **Water.** A cold sun, a dark blue-green sky (the fog takes the sky's colour), distance haze and a field of drifting marine snow (`Content/Fx/MarineSnow.avfx`).

Nothing here makes the models game-ready: collision, rigging, animation and walkability are untested.

