# Projects and assets

A project is a folder with an `.aproject` file:

    MyGame/
      MyGame.aproject       settings: startup scene, window, quality, plugins
      Content/              everything the game uses
      Config/  Source/  Saved/  Intermediate/

## Assets and GUIDs

Every file under `Content/` that the engine understands is an asset. The asset
database gives each one a permanent GUID, stored in a `.ameta` file next to
it. **References between assets are by GUID, not by path**, so renaming or
moving a file never breaks a scene. Commit the `.ameta` files with the assets.

| Extension | Asset |
|---|---|
| `.ascene` | A scene: entities and their components |
| `.aprefab` | A reusable entity tree |
| `.luau` | A script |
| `.abp` | A Blueprint |
| `.aaction`, `.amapping` | An input action; a mapping context |
| `.gltf`, `.glb` | A model (with its textures) |
| `.png`, `.jpg`, `.tga`, `.hdr` | A texture |
| `.wav`, `.ogg`, `.flac`, `.mp3` | A sound |
| `.aatlas`, `.atileset`, `.atilemap` | A sprite atlas, a tileset, a tilemap (see [2D](05-2d.md)) |

## Scenes, entities and components

A scene is entities with components. A component is plain data (a reflected C++
struct); `Transform`, `Camera`, `Tags`, `ModelRenderer`, `ScriptComponent` and
the rest are listed in the [API reference](08-api-reference.md) with every
field, its range and its unit. Scenes are JSON, so they diff and merge.

A **prefab** is an entity tree saved as an asset; a scene holds instances of it
and records only the fields you overrode.

## The Inspector

Select an entity and the Inspector draws its components from their
reflection: a field's tooltip, range, unit and group come from the
`AETHER_REFLECT` that declares it. Plugins can replace the widget for a type
([chapter 7](07-extending.md)).
