"""Writes Ashen Frontier's items (.aitem), recipes (.arecipe) and achievements (.aachievement) into Content/, each with its .ameta (a stable GUID from its name).

The pack is Kael's Inventory: Scrap, Component and Sample stack to 99, the Generator Core is a single part. The Generator Core recipe is the fab
bench's: 4 Scrap and 2 Components make 1 Generator Core.

    python tools/make_items.py
"""
import json
import pathlib
import uuid

CONTENT = pathlib.Path(__file__).resolve().parent.parent / "Content"
NAMESPACE = uuid.UUID("a5e17f3c-0c77-4b1a-9a0e-5a1e7f20a111")  # this game's, so a name always gives the same GUID

ITEMS = {
    "Scrap": {"name": "Scrap", "max_stack": 99},
    "Component": {"name": "Component", "max_stack": 99},
    "Sample": {"name": "Sample", "max_stack": 99},
    "GeneratorCore": {"name": "GeneratorCore", "max_stack": 1},
}

ACHIEVEMENTS = {
    "FirstHaul": {"name": "FirstHaul", "title": "First Haul", "description": "Take your first piece of salvage.", "target": 1},
    "Scavenger": {"name": "Scavenger", "title": "Scavenger", "description": "Take 10 pieces of salvage.", "target": 10},
    "Fabricator": {"name": "Fabricator", "title": "Fabricator", "description": "Craft a Generator Core at the fab bench.", "target": 1},
    "LightsOn": {"name": "LightsOn", "title": "Lights On", "description": "Start the outpost generator.", "target": 1},
    "PackMule": {"name": "PackMule", "title": "Pack Mule", "description": "Repair MULE-3.", "target": 1},
    "DistressCall": {"name": "DistressCall", "title": "Distress Call", "description": "Reach the distress relay.", "target": 1},
    "GreenThumb": {"name": "GreenThumb", "title": "Green Thumb", "description": "Restore the living dome.", "target": 1},
    "Scrapper": {"name": "Scrapper", "title": "Scrapper", "description": "Disable 3 sentinels.", "target": 3},
    "AssemblyLine": {"name": "AssemblyLine", "title": "Assembly Line", "description": "Start the assembly line.", "target": 1},
}

RECIPES = {
    "GeneratorCore": {"name": "GeneratorCore", "title": "Generator Core",
                      "inputs": [{"item": "Scrap", "count": 4}, {"item": "Component", "count": 2}],
                      "outputs": [{"item": "GeneratorCore", "count": 1}]},
}


def write(folder, name, extension, importer, data):
    path = CONTENT / folder / (name + extension)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")
    meta = {"$type": "AssetMeta", "$v": 1, "guid": str(uuid.uuid5(NAMESPACE, folder + "/" + name)), "importer": importer, "importer_version": 1,
            "settings": {}, "labels": [], "source_hash": ""}
    pathlib.Path(str(path) + ".ameta").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    for name, item in ITEMS.items():
        write("Items", name, ".aitem", "ItemDefinition", item)
    for name, recipe in RECIPES.items():
        write("Recipes", name, ".arecipe", "RecipeDefinition", recipe)
    for name, achievement in ACHIEVEMENTS.items():
        write("Achievements", name, ".aachievement", "AchievementDefinition", achievement)
    print("wrote", len(ITEMS), "items,", len(RECIPES), "recipes and", len(ACHIEVEMENTS), "achievements")
