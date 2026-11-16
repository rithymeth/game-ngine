# Abyss Protocol

**The deeper you descend, the less you can trust what you hear.**

Underwater exploration / survival mystery for Aether Engine. Stylized 3D, single-player first.

## Concept pillars

1. **The ocean listens** — Sound is both a tool and a threat. Active sonar reveals the world but attracts predators. Machinery, collisions, and movement create acoustic signatures.
2. **Darkness is a resource** — Lights improve visibility and navigation but increase detection and drain batteries. Sometimes darkness protects you.
3. **Depth changes the rules** — Greater depths increase pressure, degrade equipment, and introduce specialized creatures.

Core loop: launch from Haven-01 research hub → explore a region with submarine + EVA diver → manage acoustic signature, lights, oxygen/battery/pressure → recover data or samples → return or establish modular habitats.

## Current prototype status

The sample contains a working simulation of the core systems:

- **Acoustic events** — continuous noise from thrusters + discrete sonar pulses that decay over time.
- **Creature AI** — one sound-sensitive creature that raises awareness from noise/light, investigates when alerted, and drifts back to home when calm.
- **Pressure / resources** — simple depth-based battery drain and oxygen consumption.
- **glTF loading** — `Game::LoadModel` uses `aether::assets::LoadGltf` and reports mesh/vertex/material counts plus axis-aligned bounds.
- Console demo in `main.cpp` that runs a 10-second simulation and can load a model if you pass a path.

This is still a pure C++ simulation library (no GPU upload or renderer yet). The loaded `GltfScene` is kept so a future renderer can consume it.

## Build integration (required)

Add this single line near the other samples in the root `CMakeLists.txt` (after `add_subdirectory(samples/frozen_api_game)`):

```cmake
add_subdirectory(samples/abyss_protocol)
```

Then configure and build as usual:

```powershell
cmake --build build --config RelWithDebInfo --target abyss_protocol
```

Run with an optional model:

```powershell
.\abyss_protocol.exe path\to\submarine.glb
```

## glTF integration

Aether’s loader (`aether::assets::LoadGltf`) already handles:
- Positions, normals, UVs, tangents, indices
- PBR metallic-roughness materials + texture paths
- Node hierarchy and world transforms
- Skins and animations (evaluate with `EvaluateAnimation` / `ComputeSkinMatrices`)
- Both `.gltf` + external `.bin` and self-contained `.glb`

The sample’s `LoadModel` currently only extracts statistics and bounds. Full GPU upload and drawing is demonstrated in the engine’s `gltf_demo` (D3D12 + bindless textures + optional skinning).

## High-quality 3D / asset models

Recommended approach for stylized, moderately detailed assets with strong silhouettes, bioluminescent accents, and atmospheric fog:

### Recommended free / high-quality sources
- **Sketchfab**: Search "stylized underwater", "sci-fi submersible", "bioluminescent coral". Many free CC-BY models; prefer ones that export clean glTF.
- **Meshy.ai**: Free underwater and sci-fi vehicle models (CC0). Good starting points for coral, rocks, and small props.
- **CGTrader free section**: Stylized nature packs and low-poly underwater environments.
- **Hugging Face**: Packs of 100+ deep-sea fish species as optimized GLB files (excellent for the creature ecology).
- **Poly Haven**: High-quality PBR textures (water caustics, ice, metal, coral-like materials).

### Technical notes for Aether
- Prefer **glTF 2.0 (.glb)** with PBR metallic-roughness workflow.
- Keep poly counts moderate for real-time (the engine has meshoptimizer support in the cooker).
- Use emissive maps for bioluminescence and interior lights.
- The engine already has glTF loading support via the asset pipeline.

**License note**: Always verify commercial-use rights before shipping. Prefer CC0 or properly licensed assets.

## Next steps

1. Add the `add_subdirectory` line above and verify the sample builds and runs the console demo (with and without a model path).
2. Replace the console `main.cpp` with a real Win32 / GLFW window + software or D3D12 renderer (copy patterns from `gltf_demo` and `samples/gem_hop`).
3. Upload the loaded `GltfScene` primitives to GPU buffers and draw them at the submarine / creature positions.
4. Expand the acoustic system into a proper ECS system that can run on the job system.
5. Add a simple research hub entity and one complete short mission (restore the silent relay).

See the original design discussion for the full story (Protocol Zero, MIRA AI, Neris-9 regions) and longer-term scope.
