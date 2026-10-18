# The Aether manual

How to make a game with Aether, in the order you'll need it. Start with the
[first playable build](01-getting-started.md), then use the chapters below as
the workflow grows. The chapters describe current capabilities; build specs in
[../design](../design) describe how they work inside.

1. [Getting started](01-getting-started.md): build and play the Coin Run sample from a clean Windows checkout.
2. [Projects and assets](02-projects-and-assets.md): the folders, GUIDs, scenes, prefabs, and what the asset database does.
3. [Scripting](03-scripting.md): Luau scripts and Blueprints.
4. [Input](04-input.md): actions, mapping contexts and bindings.
5. [Making a 2D game](05-2d.md): sprites, tilemaps, 2D physics, lights, the platformer controller.
6. [Packaging and shipping](06-packaging.md): cooking, `.apak` archives, the player, patches, DLC, encryption.
7. [Plugins and editor extensions](07-extending.md): modules, panels, menus, property drawers, editor scripts.
8. [The API reference](08-api-reference.md): every component and type, generated from the code.
9. [Gameplay](09-gameplay.md): tags, attributes, effects and abilities.
10. [Inventory and items](10-inventory.md): the inventory genre kit.
11. [Interaction](11-interaction.md): the interaction genre kit.
12. [Quests](12-quests.md): the quests genre kit.
13. [Upgrading](13-upgrading.md): what to check when moving to a newer engine.
14. [Plugin registry and marketplace format](14-plugin-marketplace.md): local packages, catalog metadata and the trust model.

Also: [Support policy](support_policy.md), a proposal for how long versions are supported.

Also: [API stability](api_stability.md), what "stable", "experimental" and "internal" mean and how to deprecate.
The [Core API inventory](core_api_inventory.md) records current ownership and threading contracts.

The sample projects are the templates: **Blank**, **First Person**, **Third
Person**, **Top Down**, **Vehicle** and **2D Platformer**, in the editor's
**New Project** tool. Each one is a small game that runs, so read them as
examples as well as starting points.
