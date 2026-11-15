"""Writes Ashen Frontier's items (.aitem) and recipes (.arecipe) into Content/, each with its .ameta (a stable GUID from its name).

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
    print("wrote", len(ITEMS), "items and", len(RECIPES), "recipes")
