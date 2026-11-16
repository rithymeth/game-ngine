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
- The boat, creatures, rocks, Haven, the relay, and the cache are glTF meshes in [assets/](assets). The boat is `hull.glb` unless you pass another file or set `AETHER_ABYSS_MODEL`. This view uses base color only. A restored relay pulses green, and the cache goes dark once taken.
- No model found: an orange placeholder hull is drawn instead.
- **Run**: leave Haven, restore the silent relay, then get home. `F` fires one sonar ping and spends battery. For a few seconds you can see farther and a bright marker points at the objective. After it fades, a dim marker keeps the last heading. It does not update until you ping again, and it clears when the relay answers. Lights show what is nearby, spend battery, and give you away. Two creatures listen. Rocks are solid even when you cannot see them.
- A rock hit also opens a leak. Oxygen falls faster until you reach the cache, which seals it, patches some hull, and refills battery. The creatures patrol, and when they wake they swim to the last sound they heard, not to you. A red mark is that spot. Go quiet and leave it. A ping only wakes a creature inside the pulse. Current streaks show under lights or a ping.
- Once the relay is restored it keeps calling. A green ring marks that sound, and a creature inside it comes to the mast. Leave before it arrives. Deeper than 26 metres the title says `PRESSURE` and the battery drains. The relay sits in that band, so do not wait there.

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
