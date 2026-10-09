# AETHER-01 Asset Foundry — Last Light

A production-oriented, engine-ready asset drop for **AETHER-01: First Contact**. The pack contains authored mesh geometry in glTF 2.0, binary buffers, stable engine `.ameta` identities, an explorable canyon map scene, a catalog, and a deterministic procedural source builder.

## Deliverables

| Asset | Scale / detail | Visual direction |
| --- | --- | --- |
| `Content/Characters/Kael/Kael_Recon.gltf` | 53 mesh parts, about 4,800 triangles, 1.9 m tall | Recon armor, layered plates, helmet optics, pressure suit, pack, pouches, weapon-ready silhouette |
| `Content/Weapons/AEGIS/AEGIS_PrecisionCarbine.gltf` | 24 mesh parts, about 760 triangles | Carbine receiver, shroud, muzzle brake, ion ring, magazine, optic, sights, grip and charge details |
| `Content/Environments/LastLight/LastLight_Valley.gltf` | 96 m x 96 m, about 16,400 triangles and 135 named pieces | Eroded valley, braided meltwater, basalt spires and talus, transit bridge, gate, signal obelisks, distant citadel |
| `Content/Scenes/LastLight_Valley.ascene` | Engine scene entry point | A single organized environment entity references the map asset |

Meshes are split into named objects for editing and reuse. Surfaces use glTF metallic-roughness materials with distinct ceramic, carbon, alloy, water, stone, and amber signal values. The character is currently a static mesh because this engine project’s renderer does not yet render skeletal animation.

## Repository layout

```text
games/AETHER-01/
├── Content/
│   ├── Characters/Kael/Kael_Recon.gltf[.ameta] + .bin
│   ├── Weapons/AEGIS/AEGIS_PrecisionCarbine.gltf[.ameta] + .bin
│   ├── Environments/LastLight/LastLight_Valley.gltf[.ameta] + .bin
│   └── Scenes/LastLight_Valley.ascene[.ameta]
├── ASSET_CATALOG.json
├── ASSET_BUILD_GUIDE.md
└── tools/
    ├── build_assets.py
    └── validate_assets.py
```

## Import and use

1. Open `games/AETHER-01/AETHER-01.aproject` in the Aether editor. The `.ameta` files give imported assets stable GUIDs; keep them beside their source files.
2. Open `Content/Scenes/LastLight_Valley.ascene` or assign `Content/Environments/LastLight/LastLight_Valley.gltf` to a `ModelRenderer` entity.
3. Use the character GLTF as a static model. It is centered at the feet and scaled for the existing 1.8 m player capsule.
4. The AEGIS mesh faces down local -Z. Orient its root toward the aim direction when attaching it to a character. The asset is art-only; existing weapon firing, inventory, and pickup code remain game logic.
5. Before shipping the valley as a combat level, add collision primitives or the project’s supported static collision, navigation, encounter triggers, and level lighting. This mesh pack does not claim those systems are configured.

## Rebuild

From this directory run:

```sh
python3 tools/build_assets.py
python3 tools/validate_assets.py
```

The builder only uses the Python standard library. It regenerates deterministic glTF, external `.bin` geometry buffers, catalog data, and stable `.ameta` GUIDs. The validator checks buffer and accessor bounds, triangle topology, material indices, metadata GUID uniqueness, and scene-to-model references. Each model is self-contained except for its adjacent buffer. The files are glTF 2.0 JSON plus binary data; no image or concept render is substituted for 3D geometry.

## Map design / build structure

The map is a 96 m playable footprint with a 65 x 65 triangulated heightfield. A carved, bending channel routes from the southern entry toward the northern exit. Rock landmarks cluster along the rim, leaving a readable centerline. The bridge, gate, and citadel terminate the route; amber wayfinders mark progression. The catalog distinguishes gameplay-scale assets from map landmarks. The generated valley is a visual and level-art base; collision and mission scripting should be authored as separate engine entities.

## Production integration notes

- These are editable mesh assets with named parts and PBR material assignments, sized and organized for engine import. They are an integration-ready art foundation, not final AAA content: high-resolution sculpt bakes, texture maps, authored skeletal rig and animation, collision, navmesh, and mission logic are not included.
- Kael is intentionally a static mesh until skeletal animation is supported by the current project renderer. The carbine is an art asset; attach it to the character and connect it to gameplay weapon code separately.
- The valley is one glTF model with modular named mesh parts. Use the scene entry point for inspection, then split or instance pieces as needed for streaming, collision, and gameplay optimization.
- Keep each `.ameta` beside its source so stable asset identities survive imports. Run the validator after rebuilding or modifying the pack.
