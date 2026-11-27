# Lanternwake art direction and provenance

## Direction

Original hand-painted storybook fantasy for Lanternwake. The environment uses ink-blue and moss-teal shadows, with amber illumination identifying light and restoration. Smooth, quiet ground keeps small sprites readable; foliage and masonry concentrate detail along the boundary. The traveler has a clear turquoise hood, golden scarf, and brass lantern. The indigo wraith uses pale cyan eyes and curved wisps. Restored shrines repeat the moss, aged stone, brass, and amber palette.

All four finished PNGs were generated using the built-in image_gen tool on 2026-11-27. The three object sprites were requested with true transparent backgrounds. Their original RGBA pixels and alpha are preserved. No raster edits, compositing, repainting, resampling, or programmatically drawn replacement art were applied. The environment is an RGB image.

## Delivered assets

| File | Source dimensions | Format | Purpose |
| --- | --- | --- | --- |
| forest.png | 1536 × 1024 | RGB PNG | Full environment, scale to 1280 × 800 logical viewport |
| keeper.png | 1047 × 1501 | RGBA PNG | Player, approximately 54 × 78 logical pixels |
| wraith.png | 1254 × 1254 | RGBA PNG | Enemy, approximately 60 × 65 logical pixels |
| shrine.png | 1254 × 1254 | RGBA PNG | Restored shrine, approximately 90 × 110 logical pixels |

Sprite dimensions are the delivered canvas dimensions. Transparent margins remain unchanged. Engine rendering may choose a source rectangle or a display size; preserving the PNG originals allows reuse at higher sizes.

## Environment integration

The generated landmark centers are visually approximately:

| Landmark | Normalized center | At 1280 × 800 |
| --- | --- | --- |
| Left shrine ring | (0.254, 0.358) | (325, 286) |
| Right shrine ring | (0.752, 0.358) | (963, 286) |
| Lower shrine ring | (0.504, 0.686) | (645, 549) |
| Exit doorway floor | (0.500, 0.200) | (640, 160) |

These are visual estimates for game integration, not baked navigation data. The broad connected clearing supports free movement. Trees, heavy roots, and ruins define the visual boundary.

## Generation records

The original generated files remain in the generation output directory. The project copies are the runtime assets.

- forest.png: exec-639b06db-9391-4f09-89f9-189a028081f0.png
- keeper.png: exec-d8eec83f-6aeb-4791-9b54-918d8d4921b5.png
- wraith.png: exec-67103df3-9456-4f6d-bd34-f1b01d2fa05a.png
- shrine.png: exec-e1c7ef32-f206-438c-bad0-6a6031b74908.png

## Final prompts

### forest.png

```text
Use case: stylized-concept
Asset type: final production background for the original premium 2D exploration game LANTERNWAKE, logical viewport 1280 by 800.
Primary request: One beautiful wide landscape overhead fantasy woodland level, roughly 16:10 (1536x1024 landscape acceptable). A truly playable whole arena viewed from above, no horizon. Hand-painted storybook fantasy with refined painterly textures, deep ink-blue shadows, saturated moss teal, restrained warm amber bioluminescent details and theatrical soft lighting. A single connected broad open central clearing and connected looping paths, the central eighty percent mostly unobstructed smooth dark forest floor and sparse short moss, with only subtle irregular stones and roots at its periphery. Tree trunks, lush foliage, big moss-coated roots, ferns and broken ancient stonework densely frame the extreme outside edges only, forming a beautiful decorative vignette, never obscuring the central play space.
Composition landmarks: Three clearly EMPTY flat circular ancient stone shrine bases, each about seven percent of the frame width, embedded in the ground at normalized coordinates (26 percent from left, 38 percent from top), (76 percent from left, 38 percent from top), and (51 percent from left, 70 percent from top). These are empty low stone rings with flat empty centers, no statues or columns on them. At (50 percent from left, 20 percent from top) sits a weathered broken exit arch of stone, amber motes around it, with a clear dark walkable opening. Gentle paths connect all landmarks around the spacious central clearing. Landmark positions are crucial.
Visual tone: magical twilight, elegant atmospheric game art, precise shapes, jewel-like tiny luminous plants at edges, subtle mist only at borders. Highly detailed tactile brushwork on forest edge foliage and ancient masonry, with deliberately quieter ground so small game sprites remain readable. Orthographic overhead sixty-degree view, consistent grounded perspective.
Constraints: Only this background, full bleed. No characters, no enemies, no people, no animals, no UI, no lettering, no logos, no watermarks. Do not divide into panels. No sky, no horizon, no high foreground foliage across the playable clearing, no floating islands. No prominent objects beyond the specified stone bases and arch in the central play space.
```

### keeper.png

```text
Use case: stylized-concept
Asset type: final isolated transparent player character sprite for the original premium 2D exploration game LANTERNWAKE.
Primary request: One centered full-body original little wandering lantern keeper, viewed from overhead at a sixty-degree angle, a three-quarter view facing slightly down and right. A large deep turquoise hood and softly layered deep turquoise cloak, face hidden in warm dark shadow with just a subtle suggestion of nose, rich golden scarf curling around collar, soft leather boots, two legs visibly separate. In the hand on the image's right side, carry a small exquisite brass lantern with a warmly glowing amber glass center; keep lantern close enough to silhouette to read as part of the sprite. The traveler is a mysterious charming adult fantasy wanderer, no recognizable existing character.
Style/medium: premium hand-painted storybook fantasy game asset, softly modeled near-3D volume, visible exquisite brush texture in folds, ink-blue deep shadows, rich teal highlights with controlled warm amber reflected light from the lantern. Strong iconic and legible silhouette, charming proportions with slightly oversized hood, soft asymmetry, high-quality material painting.
Composition/framing: Exactly one character centered, entire body and lantern visible, comfortably within central 90 percent of the canvas, roughly portrait silhouette. Neutral idle pose suitable for sprite bob and movement animation. Will be drawn about 54x78 pixels in game, prioritize large readable shapes over microdetail.
Background: genuinely transparent alpha background, no floor, no ground, no cast shadow and no baked backdrop or checkerboard.
Constraints: No sprite sheet, no second figure, no multiple angles, no text, no watermark, no scene or scenery. Keep uncluttered and production-ready. Preserve clean translucent antialiased edges.
```

### wraith.png

```text
Use case: stylized-concept
Asset type: final isolated transparent enemy spirit sprite for the original premium 2D exploration game LANTERNWAKE.
Primary request: Exactly one original small floating woodland wraith, centered, full creature visible, overhead sixty-degree view facing slightly downwards. Its body is an inky indigo tapering mantle of softly tattered ribbon-like wisps, a big simple shadowed face with exactly two dim pale cyan eyes, long curved claw-like vapor wisps reaching outward to frame body. Eerie but enchanting and approachable, never gruesome. The lower body dissolves into several distinct elegant tapered ribbons, all readable within a compact silhouette. No feet or equipment.
Style/medium: premium hand-painted storybook fantasy game asset with softly modeled near-3D volume, painterly tactile fabric/vapor, ink-blue and indigo shadows, subtle desaturated teal edge lighting, very restrained cyan glow at eyes. Same visual palette as a teal-and-amber magical twilight forest. Not glossy plastic or a flat icon.
Composition/framing: One compact centered creature, its entire silhouette inside the central ninety percent of square canvas. Distinct eyes and clean broad silhouette readable at about 60x65 pixels. Hovering neutral pose for simple bob animation, about as wide as it is tall.
Background: genuinely transparent alpha background. No ground, no cast shadow, no environment, no baked backdrop or checkerboard. Any translucent vapor belongs only to the creature and ends cleanly at silhouette.
Constraints: No sprite sheet, no multiple angles, no second creature, no text, no logos, no watermark, no scene. No gore or horror violence. Preserve clean transparent antialiased edges.
```

### shrine.png

```text
Use case: stylized-concept
Asset type: final isolated transparent world-object sprite for the original premium 2D exploration game LANTERNWAKE.
Primary request: Exactly one ornate ancient forest lantern shrine, fully restored and glowing, shown from overhead at a sixty-degree angle. A short squat circular stone plinth supporting a carved moss-coated stone pedestal and an elegantly faceted amber crystal suspended in a rounded brass-and-stone cage. The warm brilliant inner light is the focus. Simple readable shape, strong large amber center and aged teal-grey stone, tiny tasteful teal moss on corners, restrained carved geometric ornament. It belongs in a beautiful hand-painted storybook woodland level at twilight, surrounded by deep ink-blue shadows and rich moss teal.
Style/medium: premium painterly fantasy game prop with softly modeled near-3D volume, exquisite tactile stone and moss brush texture, warm amber glow reflecting on inner stone facets, elegant and readable at small sizes. A very low circular foot so it fits on a flat circular shrine base in a level; shrine is upright but compact, about as tall as it is wide, no huge cathedral spire.
Composition/framing: Exactly one shrine centered and fully visible, hovering in isolation with no ground or scene. Entire object plus subtle luminous edge within central eighty-five percent of square canvas. Sprite will be displayed about 90x110 pixels.
Background: genuinely transparent alpha, no floor, no ground, no cast shadow, no baked backdrop and no checkerboard. Glow confined close to crystal and edges.
Constraints: No people, no creature, no extra props, no scenery, no text, no logos, no watermarks, no multiple views or sprite sheet. Preserve clean antialiased transparent edges.
```
