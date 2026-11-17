#!/usr/bin/env python3
"""Builds an Aether scene from the 128 Abyss Protocol models and renders it with the player.

    python build_haven.py --models DIR --work WORK --catalog              # every model in a grid, to see them all
    python build_haven.py --models DIR --work WORK --size 1920x1080       # the Haven scene: the colony, the docks, the seabed, the ruins and the creatures

DIR holds the GLBs (AP-001_Minnow_Submarine.glb ...); the models are not in the repository (they are Meshy exports and about 840 MB). ASSET_LIST.csv (id, name,
category, file name) is next to this script. The scene is cooked and filmed with the player on Vulkan (lavapipe and xvfb on a machine without a GPU).
"""
import argparse
import csv
import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
import uuid

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(HERE))
import haven_models as hm  # noqa: E402

NAMESPACE = uuid.UUID("a3b0c1d2-7e44-4f10-9c21-5d6e7f809a1b")


def guid_for(key):
    return str(uuid.uuid5(NAMESPACE, key))


def quat_y(theta):
    return [0.0, math.sin(theta / 2), 0.0, math.cos(theta / 2)]


def look(from_pos, to_pos):
    """Yaw and pitch in degrees for the player's camera path (yaw 0 faces -Z and turns left; pitch negative looks down)."""
    dx, dy, dz = (to_pos[i] - from_pos[i] for i in range(3))
    return math.degrees(math.atan2(-dx, -dz)), math.degrees(math.atan2(dy, math.hypot(dx, dz)))


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, check=True, **kwargs)


def asset_meta(content, rel, importer):
    meta = {"$type": "AssetMeta", "$v": 1, "guid": guid_for("asset/" + rel), "importer": importer, "importer_version": 1, "labels": [], "settings": {}, "source_hash": ""}
    (content / (rel + ".ameta")).write_text(json.dumps(meta, indent=2) + "\n")


class SceneBuilder:
    def __init__(self):
        self.scene = {"$type": "Scene", "$version": 1, "entities": []}

    def add(self, name, components, pos=(0, 0, 0), rot=(0, 0, 0, 1), scale=(1, 1, 1), tags=()):
        comps = dict(components)
        comps["EntityName"] = {"$v": 1, "value": name}
        comps["Transform"] = {"$v": 2, "position": [round(v, 4) for v in pos], "rotation": [round(v, 6) for v in rot], "scale": list(scale)}
        if tags:
            comps["Tags"] = {"$v": 1, "names": list(tags)}
        self.scene["entities"].append({"components": comps, "guid": guid_for("entity/" + name)})


def model_component(path):
    return {"ModelRenderer": {"$v": 1, "asset_path": path, "model": ""}}


def read_assets(models_dir):
    listing = pathlib.Path(models_dir) / "ASSET_LIST.csv"
    if not listing.exists():
        listing = HERE / "ASSET_LIST.csv"   # the repository's copy (ids, names, categories and file names only)
    rows = list(csv.DictReader(open(listing, newline="", encoding="utf-8-sig")))
    for r in rows:
        r["file"] = pathlib.Path(models_dir) / pathlib.Path(r["filename"]).name
        assert r["file"].exists(), f"missing {r['file']}"
    return rows


def lights_and_camera(sb, camera_pos, camera_target, fov=50.0, sun=3.0, sky=1.0, fog=0.0, fog_color_sky=None):
    yaw, pitch = look(camera_pos, camera_target)
    # the camera entity's own rotation: yaw about Y, then pitch about X (the film's camera path overrides it when there is one)
    hy, hp = math.radians(yaw) / 2, math.radians(pitch) / 2
    rot = [math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp), -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp)]
    sb.add("MainCamera", {"Camera": {"$v": 1, "projection": "Perspective", "fov_degrees": fov, "near_plane": 0.1, "far_plane": 1500.0, "ortho_height": 10.0, "priority": 0}}, pos=camera_pos, rot=rot)
    sb.add("Sun", {"DirectionalLight": {"$v": 1, "color": [1.0, 0.97, 0.92], "intensity": sun, "cast_shadows": True}}, rot=[-0.26, 0.9, 0.1, 0.34])
    sb.add("Sky", {"SkyLight": {"$v": 1, "color": [1.0, 1.0, 1.0], "intensity": sky, "environment": "", "environment_rotation_degrees": 0.0}})


def prepare_models(rows, content, size_for, max_texture, heroes=(), tile=512, **fit_kwargs):
    """Writes each model into Content/Models/<id>.glb at the size `size_for(row)` gives (x, y, z metres). A hero keeps its own textures (cut to `max_texture`); every
    other model shares the base-colour atlases (haven_models.py says why). Returns {id: (x, y, z) metres}."""
    models = content / "Models"
    models.mkdir(parents=True, exist_ok=True)
    atlas = hm.Atlas(models, tile=tile)
    sizes = {}
    for r in rows:
        dst = models / f"{r['asset_id']}.glb"
        options = dict(fit_kwargs)
        extra = size_for(r)
        size, extra_options = (extra if isinstance(extra[0], tuple) else (extra, {}))
        options.update(extra_options)
        hero = r["asset_id"] in heroes
        sizes[r["asset_id"]] = hm.fit(r["file"], dst, size, max_texture=max_texture, atlas=None if hero else atlas, **options)
        asset_meta(content, f"Models/{r['asset_id']}.glb", "Model")
    for name in atlas.save():
        asset_meta(content, f"Models/{name}", "Texture")
    print(f"{len(rows) - len(heroes)} models share {len(atlas.images)} atlas(es) of {atlas.size}x{atlas.size}; {len(heroes)} heroes keep their own textures", flush=True)
    return sizes


# --- sizes: the longest side of each model, in metres (a diver is 1.9 m; a submarine is not) ------------------------------------------------------------------------
SIZE = {
    1: 11, 2: 26, 3: 6, 4: 6, 5: 12, 6: 9, 7: 8, 8: 10, 9: 9, 10: 10, 14: 10, 15: 12, 16: 3.2, 17: 8, 18: 22, 19: 20, 20: 7, 21: 16, 22: 22, 23: 9, 24: 10, 25: 30, 26: 12,
    27: 16, 28: 24, 29: 14, 30: 26, 31: 18, 32: 4, 33: 1.9, 34: 0.9, 35: 1.3, 36: 2.2, 37: 1.3, 38: 2.6, 39: 9, 40: 36, 41: 6, 42: 5, 43: 3, 44: 2.2, 45: 7, 46: 9, 47: 9,
    48: 16, 49: 14, 50: 20, 51: 28, 52: 2.2, 53: 14, 54: 14, 55: 8, 56: 4, 57: 14, 58: 3, 59: 4, 60: 1.6, 61: 12, 62: 14, 63: 12, 64: 12, 65: 12, 66: 16, 67: 14, 68: 14,
    69: 12, 70: 14, 71: 5, 72: 2.4, 73: 5, 74: 9, 75: 7, 76: 4, 77: 8, 78: 1.8, 79: 24, 80: 34, 81: 9, 82: 10, 83: 10, 84: 14, 85: 7, 86: 7, 87: 20, 88: 30, 89: 3.5, 90: 10,
    91: 5, 92: 2.2, 93: 1.4, 94: 1.6, 95: 10, 96: 12, 97: 16, 98: 10, 99: 18, 100: 10, 101: 9, 102: 14, 103: 3, 104: 6, 105: 4, 106: 6, 107: 10, 108: 8, 109: 3.6, 110: 3,
    111: 12, 112: 11, 113: 12, 114: 16, 115: 12, 116: 20, 117: 20, 118: 18, 119: 9, 120: 11, 121: 70, 122: 18, 123: 5, 124: 8, 125: 9, 126: 22, 127: 34, 128: 10,
}
MANTA_CROSS_SECTION = 9.0   # the Manta's three hull sections (11, 12, 13) are fitted by their cross-section so they meet end to end
# Keep their own textures (base colour, normal map and metallic-roughness: 3 of the renderer's 128 texture slots each); every other model shares the atlases.
HEROES = {1, 2, 4, 25, 33, 51, 53, 54, 57, 79, 80, 88, 121, 123}


def number(asset_id):
    return int(asset_id.split("-")[1])


def size_for(row):
    n = number(row["asset_id"])
    if n in (11, 12, 13):
        return ((MANTA_CROSS_SECTION, MANTA_CROSS_SECTION, 100.0), {"by": "width"})
    return (SIZE[n],) * 3


# --- plain meshes: the seabed and the slabs things stand on ---------------------------------------------------------------------------------------------------------------
def write_plain_glb(path, positions, normals, indices, color, roughness=0.95, metallic=0.0):
    import struct as st
    pos = b"".join(st.pack("<fff", *p) for p in positions)
    nor = b"".join(st.pack("<fff", *n) for n in normals)
    idx = b"".join(st.pack("<I", i) for i in indices)
    binary = pos + nor + idx
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
           "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
           "materials": [{"pbrMetallicRoughness": {"baseColorFactor": list(color) + [1.0], "metallicFactor": metallic, "roughnessFactor": roughness}}],
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": lo, "max": hi},
                         {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
                         {"bufferView": 2, "componentType": 5125, "count": len(indices), "type": "SCALAR"}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pos)}, {"buffer": 0, "byteOffset": len(pos), "byteLength": len(nor)},
                           {"buffer": 0, "byteOffset": len(pos) + len(nor), "byteLength": len(idx)}],
           "buffers": [{"byteLength": len(binary)}]}
    hm.write_glb(path, doc, binary)


def seabed_height(x, z):
    """Gentle swells, a few centimetres to half a metre: enough that the seabed is not a table."""
    return 0.32 * math.sin(x * 0.045 + 1.3) * math.cos(z * 0.037) + 0.18 * math.sin((x + z) * 0.11) + 0.1 * math.sin(x * 0.23 - z * 0.19)


def write_seabed(path, half=1500.0, cells=260):
    positions, normals, indices = [], [], []
    step = 2 * half / cells
    for j in range(cells + 1):
        for i in range(cells + 1):
            x, z = -half + i * step, -half + j * step
            y = seabed_height(x, z)
            e = 0.5
            nx, nz = seabed_height(x - e, z) - seabed_height(x + e, z), seabed_height(x, z - e) - seabed_height(x, z + e)
            length = math.sqrt(nx * nx + 4 * e * e + nz * nz)
            positions.append((x, y, z))
            normals.append((nx / length, 2 * e / length, nz / length))
    for j in range(cells):
        for i in range(cells):
            a = j * (cells + 1) + i
            indices += [a, a + cells + 1, a + 1, a + 1, a + cells + 1, a + cells + 2]
    write_plain_glb(path, positions, normals, indices, (0.075, 0.095, 0.095), roughness=0.95)


def write_slab(path, width, depth, height, color, roughness=0.8, metallic=0.1):
    """A box standing on y = 0, centred on x and z, with flat-shaded sides."""
    hx, hz = width / 2, depth / 2
    faces = [((0, 1, 0), [(-hx, height, -hz), (-hx, height, hz), (hx, height, hz), (hx, height, -hz)]),
             ((0, 0, 1), [(-hx, 0, hz), (hx, 0, hz), (hx, height, hz), (-hx, height, hz)]),
             ((0, 0, -1), [(hx, 0, -hz), (-hx, 0, -hz), (-hx, height, -hz), (hx, height, -hz)]),
             ((1, 0, 0), [(hx, 0, hz), (hx, 0, -hz), (hx, height, -hz), (hx, height, hz)]),
             ((-1, 0, 0), [(-hx, 0, -hz), (-hx, 0, hz), (-hx, height, hz), (-hx, height, -hz)])]
    positions, normals, indices = [], [], []
    for normal, corners in faces:
        base = len(positions)
        positions += corners
        normals += [normal] * 4
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    write_plain_glb(path, positions, normals, indices, color, roughness, metallic)


# --- the layout ------------------------------------------------------------------------------------------------------------------------------------------------------------
class Layout:
    """Where every model stands: (id, x, z, yaw, lift above the seabed, scale). `size` gives the metres each model was fitted to (its longest side after the fit)."""

    def __init__(self, sizes, rows):
        import random
        self.rng = random.Random(20261117)
        self.sizes, self.rows = sizes, {number(r["asset_id"]): r for r in rows}
        self.items = []
        self.zones = {}

    def put(self, n, x, z, yaw=0.0, lift=0.0, scale=1.0, zone=None):
        self.items.append(dict(n=n, x=x, z=z, yaw=yaw, lift=lift, scale=scale, zone=zone))

    def extent(self, n):
        """The model's fitted size (x, y, z) in metres."""
        return self.sizes[f"AP-{n:03d}"]

    def build(self):
        put, ext, rng = self.put, self.extent, self.rng
        face = lambda x, z: math.atan2(-x, -z) + math.pi   # a model's front (+Z) towards the middle  # noqa: E731

        # 1. The colony: a hub on its foundation, a ring of buildings round it, a second ring of plant, outposts at the edge.
        foundation = ext(40)[1]
        put(40, 0, 0, zone="colony")
        put(116, 0, 0, lift=foundation, zone="colony")
        ring = [2, 22, 117, 118, 25, 28, 61, 62, 99]
        for i, n in enumerate(ring):
            a = 2 * math.pi * i / len(ring) + 0.35
            put(n, 52 * math.cos(a), 52 * math.sin(a), yaw=face(52 * math.cos(a), 52 * math.sin(a)), zone="colony")
        inner = [120, 128, 85, 83, 101, 115, 100, 5]
        for i, n in enumerate(inner):
            a = 2 * math.pi * (i + 0.5) / len(inner) + 0.1
            put(n, 30 * math.cos(a), 30 * math.sin(a), yaw=face(30 * math.cos(a), 30 * math.sin(a)), zone="colony")
        put(30, 78, -40, zone="colony"); put(3, 70, -62, yaw=0.6, zone="colony"); put(3, -70, -62, yaw=-0.6, zone="colony"); put(3, 78, 20, yaw=1.6, zone="colony")
        put(51, -72, -88, zone="landmark")   # the silent relay

        # 2. The dock: the drydock's pressure gate, the crane and the walkway, the submarines moored in a row, the Manta laid out in three pieces, the carrier behind.
        put(127, 0, 92, zone="dock"); put(31, -60, 100, yaw=math.pi / 2, zone="dock"); put(87, 52, 100, zone="dock"); put(20, 26, 78, zone="dock")
        row_z = 128
        x = -62
        for n in (1, 79, 80, 123):
            width = max(ext(n)[0], ext(n)[2] * 0.0 + ext(n)[0])
            put(n, x + width / 2, row_z, yaw=0.0, zone="dock")
            x += width + 9
        x = -80
        for n in (11, 12, 13):          # bow, mid, stern: end to end along X, each turned a quarter so its length runs that way
            length = ext(n)[2]
            put(n, x + length / 2, 168, yaw=-math.pi / 2, zone="dock")
            x += length + 0.3
        put(121, 20, 250, zone="dock")

        # 3. The interior gallery: every walkable room that is left, on a concrete plate, in rows.
        rooms = [n for n, r in sorted(self.rows.items()) if r["category"] == "Walkable_Interiors" and n not in {item["n"] for item in self.items}]
        columns, cell = 8, 25.0
        for i, n in enumerate(rooms):
            col, row = i % columns, i // columns
            put(n, -150 - 100 + col * cell, -60 + row * cell, yaw=math.pi, zone="gallery")
        self.gallery = dict(count=len(rooms), x=-250 + (columns - 1) * cell / 2, z=-60 + ((len(rooms) - 1) // columns) * cell / 2, w=columns * cell + 12, d=(((len(rooms) - 1) // columns) + 1) * cell + 12)

        # 4. The equipment yard: everything a person carries or pilots, in a grid on a plate, the diver standing among it.
        yard = [33, 34, 35, 36, 37, 43, 44, 52, 59, 60, 71, 72, 73, 78, 92, 93, 94, 103, 104, 109, 16, 38, 41, 42, 7, 17, 110]
        for i, n in enumerate(yard):
            col, row = i % 7, i // 7
            put(n, -150 + col * 11, 150 + row * 11, yaw=math.pi + 0.3 * math.sin(i), zone="yard")
        self.yard = dict(x=-150 + 3 * 11, z=150 + 1.5 * 11, w=7 * 11 + 8, d=4 * 11 + 8)

        # 5. The ancient ruins in the north: the obelisk in a ring of its own kind.
        put(53, 0, -170, zone="ruins"); put(54, -34, -160, yaw=0.5, zone="ruins"); put(90, 36, -158, yaw=-0.5, zone="ruins"); put(126, 70, -178, zone="ruins")
        for k, a in enumerate((0.0, 2.1, 4.2)):
            put(110, 14 * math.cos(a), -170 + 14 * math.sin(a), yaw=a, zone="ruins")
        for k in range(3):
            put(32, rng.uniform(-30, 30), rng.uniform(-195, -150), yaw=rng.uniform(0, 6.28), lift=rng.uniform(2, 6), zone="ruins")

        # 6. The wilderness in the east: flora and geology scattered with a seeded hand, a wreck, resources.
        def scatter(n, count, x0, x1, z0, z1, scale=(0.8, 1.25), lift=0.0, zone="wild"):
            for _ in range(count):
                put(n, rng.uniform(x0, x1), rng.uniform(z0, z1), yaw=rng.uniform(0, 6.28), scale=rng.uniform(*scale), lift=lift, zone=zone)
        scatter(45, 12, 110, 300, -110, 90); scatter(46, 16, 110, 300, -110, 90); scatter(47, 10, 130, 300, -110, 90, scale=(0.8, 1.4)); scatter(107, 9, 130, 300, -110, 90)
        scatter(125, 10, 110, 300, -110, 90); scatter(48, 3, 190, 290, -90, 60, scale=(0.7, 1.3)); scatter(49, 7, 110, 300, -110, 90, scale=(0.6, 1.4)); scatter(77, 4, 150, 290, -90, 60)
        scatter(56, 6, 120, 300, -110, 90); scatter(91, 4, 120, 300, -110, 90); scatter(108, 2, 140, 280, 20, 90); put(50, 170, 110, yaw=2.3, lift=-1.0, zone="wild")
        scatter(52, 5, 110, 150, 100, 140)
        # 7. The creatures: on the seabed, near their nest, and in the water above the colony.
        put(55, 215, -120, zone="creatures"); put(57, 150, -150, yaw=2.4, zone="creatures"); put(4, 195, -140, yaw=3.5, zone="creatures"); put(4, 130, -130, yaw=1.1, zone="creatures")
        put(88, 40, -20 - 130, lift=26.0, yaw=1.4, zone="creatures")                    # the leviathan, high above the north edge
        for _ in range(16):
            put(58, rng.uniform(-90, 90), rng.uniform(-90, 90), yaw=rng.uniform(0, 6.28), lift=rng.uniform(7, 22), scale=rng.uniform(0.8, 1.6), zone="creatures")
        for n, count, lo_lift, hi_lift in ((74, 2, 3, 9), (75, 3, 6, 14), (105, 3, 8, 16), (124, 2, 4, 8), (106, 4, 0.0, 0.0), (76, 2, 0.0, 0.0), (89, 4, 0.0, 0.0)):
            for _ in range(count):
                put(n, rng.uniform(100, 280), rng.uniform(-120, 100), yaw=rng.uniform(0, 6.28), lift=rng.uniform(lo_lift, hi_lift), zone="creatures")


def check_layout(layout, rows):
    placed = {item["n"] for item in layout.items}
    missing = sorted(number(r["asset_id"]) for r in rows if number(r["asset_id"]) not in placed)
    assert not missing, f"models not placed: {missing}"


def cook(project_dir, name, scene_rel, paks, build):
    proj = json.loads((REPO / "samples" / "forest_valley" / "ForestValley.aproject").read_text())
    proj["name"] = name
    proj["startup_scene"] = scene_rel
    proj["always_cook"] = [scene_rel, "Models/", "Fx/"]
    (project_dir / f"{name}.aproject").write_text(json.dumps(proj, indent=2))
    if paks.exists():
        shutil.rmtree(paks)
    run([str(build / "tools" / "cook" / "aether_cook"), str(project_dir / f"{name}.aproject"), "--out", str(paks), "--config", "development"], cwd=project_dir)


def player_command(build, paks, scene_rel, width, height, extra):
    cmd = [str(build / "player" / "aether_player"), "--pak", str(paks), "--scene", scene_rel, "--backend", "vulkan", "--size", f"{width}x{height}"] + extra
    return (["xvfb-run", "-a"] + cmd) if shutil.which("xvfb-run") else cmd


def catalog(args, rows, work, build):
    project = work / "catalog_project"
    if project.exists():
        shutil.rmtree(project)
    content = project / "Content"
    (content / "Scenes").mkdir(parents=True)
    prepare_models(rows, content, lambda r: (1.8, 1.8, 1.8), args.max_texture, long_axis_z=False)
    columns, spacing = 16, 2.2
    sb = SceneBuilder()
    rows_count = math.ceil(len(rows) / columns)
    middle = ((columns - 1) * spacing / 2, (rows_count - 1) * spacing / 2)
    width = columns * spacing
    distance = (rows_count * spacing / 2) / math.tan(math.radians(25.0)) * 1.12
    lights_and_camera(sb, [middle[0], middle[1], distance + 1.0], [middle[0], middle[1] - 0.2, 0.0], fov=50.0, sun=3.2, sky=1.2)
    for i, r in enumerate(rows):
        col, row = i % columns, i // columns
        sb.add(r["asset_id"], model_component(f"Models/{r['asset_id']}.glb"), pos=(col * spacing, (rows_count - 1 - row) * spacing, 0.0))
    (content / "Scenes" / "Catalog.ascene").write_text(json.dumps(sb.scene, separators=(",", ":")))
    asset_meta(content, "Scenes/Catalog.ascene", "Scene")
    paks = work / "catalog_paks"
    cook(project, "AbyssCatalog", "Scenes/Catalog.ascene", paks, build)
    env = dict(os.environ, VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/lvp_icd.json", AETHER_RENDER_TAA="0")
    out = work / "catalog.bmp"
    run(player_command(build, paks, "Scenes/Catalog.ascene", *[int(v) for v in args.size.split("x")], ["--frames", "6", "--screenshot", str(out)]), env=env)
    print("wrote", out)


FPS = 24
DURATION = 48.0

# The camera: (second, where it stands, what it looks at), eased between; the path is resampled twice a second and never dips under the seabed.
def camera_shots(layout):
    gallery, yard = layout.gallery, layout.yard
    return [
        (0.0, (30, 120, 360), (0, 5, 60)), (5.0, (10, 70, 250), (0, 6, 60)),                                            # in from the south over the dock, to the colony
        (8.0, (-110, 20, 215), (-20, 8, 130)), (12.0, (60, 22, 190), (10, 8, 120)),                                  # along the moored submarines
        (15.0, (140, 40, 90), (20, 10, 0)), (20.0, (-30, 44, 120), (0, 12, -10)),                                    # round the colony
        (23.0, (-120, 30, 60), (gallery["x"], 3, gallery["z"])), (27.0, (gallery["x"] + 20, 26, gallery["z"] + 70), (gallery["x"], 3, gallery["z"])),   # the rooms
        (30.0, (-40, 16, -90), (0, 9, -170)), (34.0, (45, 12, -120), (0, 10, -170)),                                  # the ruins
        (37.0, (120, 14, -10), (200, 6, -90)), (41.0, (190, 10, -60), (150, 5, -150)),                                # the wilderness and the warden
        (44.0, (yard["x"] + 12, 7.0, yard["z"] + 36), (yard["x"], 1.5, yard["z"] - 2)), (46.0, (40, 90, 330), (0, 5, 40)),  # the yard, then up and away
        (48.0, (-40, 190, 420), (0, 0, 20)),
    ]


def camera_path(layout):
    shots = camera_shots(layout)
    keys = []
    for i in range(int(shots[-1][0] * 2) + 1):
        t = i * 0.5
        k = min(max(j for j, sh in enumerate(shots) if sh[0] <= t), len(shots) - 2)
        (t0, p0, g0), (t1, p1, g1) = shots[k], shots[k + 1]
        u = (t - t0) / (t1 - t0)
        u = u * u * (3 - 2 * u)
        pos = [p0[a] + (p1[a] - p0[a]) * u for a in range(3)]
        tgt = [g0[a] + (g1[a] - g0[a]) * u for a in range(3)]
        pos[1] = max(pos[1], seabed_height(pos[0], pos[2]) + 1.8)
        yaw, pitch = look(pos, tgt)
        keys.append({"t": round(t, 3), "position": [round(v, 3) for v in pos], "yaw": round(yaw, 2), "pitch": round(pitch, 2)})
    return {"fps": FPS, "keys": keys}


CAPTIONS = [(0.6, 4.6, "ABYSS PROTOCOL", 56), (6.0, 11.5, "The dock", 40), (14.0, 21.0, "Haven colony", 40), (23.0, 29.0, "43 walkable interiors", 40),
            (30.0, 36.0, "The Chorus ruins", 40), (37.0, 43.0, "The reef and its creatures", 40), (43.5, 46.0, "The equipment yard", 40), (46.3, 47.8, "128 models", 40)]


def haven(args, rows, work, build):
    project = work / "haven_project"
    if project.exists():
        shutil.rmtree(project)
    content = project / "Content"
    (content / "Scenes").mkdir(parents=True)
    heroes = {f"AP-{n:03d}" for n in HEROES}
    (content / "Fx").mkdir()
    for f in (HERE.parent / "Content" / "Fx").iterdir():
        shutil.copy2(f, content / "Fx" / f.name)
        asset_meta(content, f"Fx/{f.name}", "ParticleSystem")
    sizes = prepare_models(rows, content, size_for, args.max_texture, heroes=heroes, tile=args.tile)
    layout = Layout(sizes, rows)
    layout.build()
    check_layout(layout, rows)
    models = content / "Models"
    write_seabed(models / "seabed.glb")
    g, y = layout.gallery, layout.yard
    write_slab(models / "plate_gallery.glb", g["w"], g["d"], 0.5, (0.19, 0.21, 0.23), roughness=0.6)
    write_slab(models / "plate_yard.glb", y["w"], y["d"], 0.4, (0.17, 0.19, 0.21), roughness=0.6)
    write_slab(models / "plate_ruins.glb", 150, 70, 0.3, (0.12, 0.11, 0.15), roughness=0.5)
    write_slab(models / "plate_dock.glb", 190, 70, 0.4, (0.19, 0.21, 0.23), roughness=0.6)
    for name in ("seabed", "plate_gallery", "plate_yard", "plate_ruins", "plate_dock"):
        asset_meta(content, f"Models/{name}.glb", "Model")

    sb = SceneBuilder()
    camera = camera_path(layout)
    first = camera["keys"][0]
    lights_and_camera(sb, first["position"], (0, 5, 60), fov=55.0, sun=2.4, sky=1.6)
    # Underwater: a cold sun, a deep blue-green sky (the fog takes the sky's colour), thick distance haze and a little bloom and grain.
    entities = sb.scene["entities"]
    for e in entities:
        if "DirectionalLight" in e["components"]:
            e["components"]["DirectionalLight"].update(color=[0.62, 0.84, 1.0], intensity=3.6)
            e["components"]["Transform"]["rotation"] = [-0.5736, 0.0, 0.0, 0.8192]
        if "SkyLight" in e["components"]:
            e["components"]["SkyLight"].update(color=[0.03, 0.17, 0.27], intensity=1.5)
    sb.add("Water", {"PostProcessVolume": {"$v": 1, "global": True, "weight": 1.0, "bloom_intensity": 0.18, "vignette": 0.28, "saturation": 1.1, "chromatic_aberration": 0.05,
                                            "film_grain": 0.05, "sharpen": 0.2, "auto_exposure": 0.4, "fog_density": 0.0058, "height_fog_density": 0.004, "height_fog_falloff": 0.05,
                                            "height_fog_base": 0.0}})
    sb.add("Seabed", model_component("Models/seabed.glb"), pos=(0, 0, 0))
    sb.add("MarineSnow", {"ParticleSystem": {"$v": 1, "asset": "Fx/MarineSnow.avfx", "auto_activate": True}}, pos=(0, 24, 40))
    sb.add("GalleryPlate", model_component("Models/plate_gallery.glb"), pos=(g["x"], seabed_height(g["x"], g["z"]) - 0.1, g["z"]))
    sb.add("YardPlate", model_component("Models/plate_yard.glb"), pos=(y["x"], seabed_height(y["x"], y["z"]) - 0.1, y["z"]))
    sb.add("RuinsPlate", model_component("Models/plate_ruins.glb"), pos=(10, seabed_height(10, -170) - 0.1, -170))
    sb.add("DockPlate", model_component("Models/plate_dock.glb"), pos=(0, seabed_height(0, 118) - 0.1, 118))
    counts = {}
    for item in layout.items:
        n = item["n"]
        counts[n] = counts.get(n, 0) + 1
        ground = seabed_height(item["x"], item["z"])
        sb.add(f"AP-{n:03d}_{counts[n]}", model_component(f"Models/AP-{n:03d}.glb"), pos=(item["x"], ground + item["lift"] - 0.08, item["z"]), rot=quat_y(item["yaw"]),
               scale=(item["scale"],) * 3, tags=(item["zone"],))
    (content / "Scenes" / "Haven.ascene").write_text(json.dumps(sb.scene, separators=(",", ":")))
    asset_meta(content, "Scenes/Haven.ascene", "Scene")
    kinds = len({i["n"] for i in layout.items})
    print(f"{len(layout.items)} things placed ({kinds} different models: all 128 are in); zones: " + ", ".join(f"{z} {sum(1 for i in layout.items if i['zone'] == z)}" for z in sorted({i['zone'] for i in layout.items})), flush=True)
    paks = work / "haven_paks"
    cook(project, "AbyssHaven", "Scenes/Haven.ascene", paks, build)
    (work / "camera_path.json").write_text(json.dumps(camera, indent=1))
    if args.no_render:
        return
    if args.stills:
        stills(args, work, build, paks, layout)
        return
    film(args, work, build, paks, layout)


def stills(args, work, build, paks, layout):
    """One frame from each camera stop (the path has a key a frame apart, so frame i is stop i exactly), as JPEGs in WORK/stills."""
    from PIL import Image
    width, height = (int(v) for v in args.size.split("x"))
    shots = camera_shots(layout)
    keys = []
    for i, (_, pos, target) in enumerate(shots):
        yaw, pitch = look(pos, target)
        keys.append({"t": round(i / FPS, 4), "position": list(pos), "yaw": round(yaw, 2), "pitch": round(pitch, 2)})
    (work / "stills_path.json").write_text(json.dumps({"fps": FPS, "keys": keys}))
    out = work / "stills"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir()
    env = dict(os.environ, VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/lvp_icd.json")
    with open(work / "stills.log", "w") as log:
        cmd = player_command(build, paks, "Scenes/Haven.ascene", width, height, ["--camera-path", str(work / "stills_path.json"), "--record-dir", str(out)])
        subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    for bmp in sorted(out.glob("frame_*.bmp")):
        Image.open(bmp).convert("RGB").save(bmp.with_suffix(".jpg"), quality=92)
        bmp.unlink()
    print(f"{len(list(out.glob('*.jpg')))} stills in {out} (of {len(shots)} stops)", flush=True)


def film(args, work, build, paks, layout):
    import threading
    import time
    from PIL import Image
    width, height = (int(v) for v in args.size.split("x"))
    frames = work / "frames"
    if frames.exists():
        shutil.rmtree(frames)
    frames.mkdir()
    done = {"stop": False, "converted": 0}

    def converter():   # each frame to a JPEG as soon as it is whole (the BMPs are huge)
        while True:
            bmps = sorted(frames.glob("frame_*.bmp"))
            for bmp in (bmps if done["stop"] else bmps[:-1]):
                Image.open(bmp).convert("RGB").save(bmp.with_suffix(".jpg"), quality=93)
                bmp.unlink()
                done["converted"] += 1
            if done["stop"] and not list(frames.glob("frame_*.bmp")):
                return
            time.sleep(0.5)

    thread = threading.Thread(target=converter, daemon=True)
    thread.start()
    env = dict(os.environ, VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/lvp_icd.json")
    started = time.time()
    with open(work / "player.log", "w") as log:
        cmd = player_command(build, paks, "Scenes/Haven.ascene", width, height, ["--camera-path", str(work / "camera_path.json"), "--record-dir", str(frames)])
        print("+", " ".join(cmd), flush=True)
        result = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    done["stop"] = True
    thread.join()
    print(f"filmed {done['converted']} frames in {time.time() - started:.0f} s (player exit {result.returncode}); log: {work / 'player.log'}", flush=True)
    if result.returncode != 0 and done["converted"] < int(DURATION * FPS):
        sys.exit(result.returncode)
    out = pathlib.Path(args.out) if args.out else work / "abyss_haven.mp4"
    font = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    vf = []
    if not args.no_titles and pathlib.Path(font).exists():
        for t0, t1, text, size in CAPTIONS:
            vf.append(f"drawtext=fontfile={font}:text='{text}':fontcolor=white:fontsize={int(size * height / 720)}:x=(w-text_w)/2:y=h*0.84:alpha='if(lt(t,{t0}),0,if(lt(t,{t0 + 0.5}),(t-{t0})/0.5,if(lt(t,{t1 - 0.5}),1,if(lt(t,{t1}),({t1}-t)/0.5,0))))':shadowcolor=black@0.7:shadowx=2:shadowy=2")
    vf.append(f"fade=t=in:st=0:d=0.8,fade=t=out:st={DURATION - 1.2}:d=1.2")
    run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-framerate", str(FPS), "-i", str(frames / "frame_%05d.jpg"), "-vf", ",".join(vf), "-c:v", "libx264", "-crf", "17",
         "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(out)])
    print(f"wrote {out} ({out.stat().st_size / 1e6:.1f} MB)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--models", required=True, help="the folder with the GLBs and ASSET_LIST.csv")
    parser.add_argument("--work", required=True)
    parser.add_argument("--build", default=str(REPO / "build"))
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--max-texture", type=int, default=1024, help="textures are cut to this many pixels across (the models carry 2048)")
    parser.add_argument("--catalog", action="store_true", help="a grid of every model, one picture")
    parser.add_argument("--tile", type=int, default=512, help="the atlas tile (pixels across) each non-hero model's base colour gets")
    parser.add_argument("--out", default=None, help="the mp4 (default: WORK/abyss_haven.mp4)")
    parser.add_argument("--no-render", action="store_true", help="build and cook the scene, film nothing")
    parser.add_argument("--no-titles", action="store_true")
    parser.add_argument("--stills", action="store_true", help="one frame from each camera stop instead of the film")
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    build = pathlib.Path(args.build)
    rows = read_assets(args.models)
    if args.catalog:
        catalog(args, rows, work, build)
        return
    haven(args, rows, work, build)


if __name__ == "__main__":
    main()
