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
- Pass a `.gltf` or `.glb` and it is centered, scaled to about 4 metres, and drawn on the submarine. Base-color materials are used. Meshes over 4000 triangles are strided down so the software view stays interactive.
- No model: an orange placeholder hull is drawn instead.
- **Silent relay**: a mast sits out in the dark. It only comes back if you reach it with sonar off, lights off, and the creature calm. The window title says `RELAY RESTORED` when it answers.

| Key | Action |
| --- | --- |
| W / A / S / D | Thrust, relative to the camera |
| Q / E | Dive / rise |
| Mouse or arrows | Look |
| F | Toggle sonar (the creature hears it) |
| L | Toggle lights (the view goes dark) |
| Esc | Quit |

The window title shows depth, battery, sonar, lights, and creature awareness. The HUD bars are battery (gold) and awareness (cyan, red when alerted).

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

1. Run the window and restore the relay without waking the creature.
2. Move mesh drawing onto the D3D12 path (`gltf_demo`) so textures and skinning show up.
3. Turn acoustics into a job-friendly ECS system.
