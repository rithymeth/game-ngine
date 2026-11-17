#!/usr/bin/env python3
"""Builds, films and encodes the Woodland engine demo.

The Woodland is a world the procedural editor bakes (ProceduralEditor_OptInBakeOfAFullSizeWorld: oak, birch and dead trees over a bay). This takes that
bake, opens a clearing in it and puts the engine's newer systems to work there, all of it driven by scripts through the engine's own kits (nothing is
animated by hand):

    Weather + Time of Day  the sky rolls from a clear golden evening to a storm: sun, clouds, haze, wind, rain and lightning
    Ropes + Force Fields   a lantern swings on a rope from a gallows, pushed by the weather's wind
    Destruction + Forces   a pile of crates is blown up and breaks into pieces
    Vehicles               a jeep drives a course on its own springs, engine and tyres
    the renderer           sun shadows, SSR on the bay, fog, auto exposure, depth of field, bloom, vignette, grain, swells and whitecaps on the water

then films the scene with the player's camera path (game time advances one film frame a frame, so the physics and the weather follow the film's clock) and
encodes the frames to an mp4 with ffmpeg.

    python build_demo.py --work /tmp/woodland --size 1280x720            # the whole thing (bakes first if there is no --bake-dir)
    python build_demo.py --work /tmp/woodland --size 480x270 --out preview.mp4   # a fast look

Needs a build tree with aether_cook, aether_player and aether_tests (the bake is a test), ffmpeg, Pillow, and for a Linux run Mesa's lavapipe and xvfb.
The 297 MB bake and the video are not committed.
"""

import argparse
import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
import threading
import time
import uuid

HERE = pathlib.Path(__file__).resolve().parent
SAMPLE = HERE.parent
REPO = SAMPLE.parent.parent
sys.path.insert(0, str(REPO / "games" / "ASHEN-FRONTIER" / "tools"))
import make_models as mm  # noqa: E402  (its primitives and glb writer, borrowed)

NAMESPACE = uuid.UUID("5d1e4a3c-0b6e-4c2f-9a77-3f2c1b8e6d10")
FPS = 24
DURATION = 48.0

# --- the models, from make_models' primitives ---------------------------------------------------------------------------------
mm.PALETTE.update({
    "wood": ((0.42, 0.27, 0.15), 0.0, 0.90, None),
    "wood_dark": ((0.22, 0.14, 0.08), 0.0, 0.92, None),
    "hemp": ((0.66, 0.54, 0.34), 0.0, 0.95, None),
    "iron": ((0.10, 0.10, 0.11), 0.65, 0.45, None),
    "flame": ((0.95, 0.60, 0.20), 0.0, 0.5, (1.0, 0.62, 0.22)),
    "jeep": ((0.28, 0.36, 0.20), 0.15, 0.55, None),
    "jeep_dark": ((0.10, 0.12, 0.09), 0.2, 0.6, None),
    "tire": ((0.04, 0.04, 0.045), 0.0, 0.92, None),
    "rim": ((0.60, 0.60, 0.63), 0.85, 0.35, None),
    "glass": ((0.05, 0.08, 0.10), 0.6, 0.15, None),
    "lamp": ((0.9, 0.9, 0.8), 0.0, 0.3, (1.0, 0.95, 0.80)),
    "boat_paint": ((0.16, 0.34, 0.50), 0.0, 0.55, None),
    "boat_white": ((0.82, 0.82, 0.78), 0.0, 0.6, None),
})


def model_crate():
    m = mm.Model()
    m.box("wood", (0, 0, 0), (0.9, 0.9, 0.9))
    for y in (-0.33, 0.33):
        m.box("wood_dark", (0, y, 0), (0.94, 0.12, 0.94))
    for x in (-0.33, 0.33):
        m.box("wood_dark", (x, 0, 0), (0.12, 0.94, 0.94))
    return m


def model_lantern():
    m = mm.Model()
    m.box("iron", (0, 0.27, 0), (0.22, 0.05, 0.22))      # the cap, with the ring above it
    m.cylinder("iron", (0, 0.34, 0), 0.05, 0.06, sides=8)
    m.box("flame", (0, 0.0, 0), (0.14, 0.30, 0.14))      # the glow
    m.box("iron", (0, -0.2, 0), (0.24, 0.06, 0.24))      # the base
    for sx in (-1, 1):
        for sz in (-1, 1):
            m.box("iron", (sx * 0.105, 0.03, sz * 0.105), (0.03, 0.46, 0.03))
    return m


def model_unit_box(material):
    def build():
        m = mm.Model()
        m.box(material, (0, 0, 0), (1, 1, 1))
        return m
    return build


def model_rope_segment():
    m = mm.Model()
    m.cylinder("hemp", (0, 0, 0), 0.5, 1.0, sides=6)
    return m


def model_jeep():
    m = mm.Model()
    m.box("jeep", (0, 0.05, 0), (1.9, 0.7, 4.2))                       # body (the collider is 1.9 x 0.8 x 4.2 about the origin)
    m.box("jeep", (0, 0.50, -0.45), (1.7, 0.55, 1.9))                  # cab
    m.box("glass", (0, 0.55, -0.45), (1.74, 0.34, 1.5))
    m.box("jeep_dark", (0, 0.21, 2.12), (1.7, 0.30, 0.10))             # grille
    for sx in (-1, 1):
        m.box("lamp", (sx * 0.62, 0.24, 2.15), (0.30, 0.20, 0.08))     # headlights
        m.box("jeep_dark", (sx * 0.95, -0.15, 1.5), (0.14, 0.18, 1.0))  # wheel arches, front
        m.box("jeep_dark", (sx * 0.95, -0.15, -1.5), (0.14, 0.18, 1.0))
    m.box("jeep_dark", (0, 0.86, -0.45), (1.7, 0.06, 1.9))             # roof rack
    return m


def model_wheel():
    m = mm.Model()
    m.cylinder("tire", (0, 0, 0), 0.35, 0.25, sides=14, rot=(0, 0, 90))   # a cylinder laid so its axle runs along X
    m.cylinder("rim", (0, 0, 0), 0.19, 0.27, sides=10, rot=(0, 0, 90))
    m.box("rim", (0, 0.27, 0), (0.28, 0.10, 0.10))                         # a lug and a spoke, so a turning wheel shows that it turns
    m.box("rim", (0, 0, 0.0), (0.28, 0.34, 0.06))
    return m


def model_boat():
    """A small open motorboat about the origin, bow towards +Z, 2.0 m across and 4.4 m long (the Floater's hull is 1.0 x 0.3 x 2.2 m half-extents)."""
    m = mm.Model()
    m.box("boat_paint", (0, -0.24, 0), (1.7, 0.10, 4.2))            # the bottom
    for sx in (-1, 1):
        m.box("boat_paint", (sx * 0.92, 0.0, -0.1), (0.16, 0.56, 3.9))  # the sides
        m.box("boat_white", (sx * 0.92, 0.30, -0.1), (0.20, 0.06, 3.95))  # their rails
    m.box("boat_paint", (0, 0.0, 2.05), (1.4, 0.56, 0.5))          # the bow narrows
    m.box("boat_paint", (0, 0.0, 2.35), (0.8, 0.56, 0.4))
    m.box("boat_white", (0, 0.30, 2.30), (0.9, 0.06, 0.5))
    m.box("boat_paint", (0, 0.02, -2.05), (1.8, 0.60, 0.16))       # the transom
    m.box("wood", (0, -0.02, 0.15), (1.75, 0.10, 0.45))            # a bench
    m.box("wood", (0, -0.02, -1.0), (1.75, 0.10, 0.45))
    m.box("iron", (0, 0.10, -2.28), (0.30, 0.75, 0.34))            # the outboard motor
    m.box("iron", (0, -0.32, -2.34), (0.12, 0.42, 0.28))
    m.cylinder("rim", (0, -0.5, -2.42), 0.12, 0.05, sides=8, rot=(90, 0, 0))
    return m


MODELS = {
    "boat": model_boat,
    "crate": model_crate, "lantern": model_lantern, "plank": model_unit_box("wood"), "pallet": model_unit_box("wood_dark"),
    "rope_segment": model_rope_segment, "jeep": model_jeep, "wheel": model_wheel,
}


# --- a model from Meshy, fitted to its body ---------------------------------------------------------------------------------------
def read_glb(path):
    data = pathlib.Path(path).read_bytes()
    json_len = int.from_bytes(data[12:16], "little")
    doc = json.loads(data[20:20 + json_len])
    rest = data[20 + json_len:]  # the BIN chunk's header and bytes
    return doc, rest


def write_glb_doc(path, doc, rest):
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    total = 12 + 8 + len(text) + len(rest)
    pathlib.Path(path).write_bytes(b"glTF" + (2).to_bytes(4, "little") + total.to_bytes(4, "little") + len(text).to_bytes(4, "little") + b"JSON" + text + rest)


def glb_bounds(doc):
    lo, hi = [1e30] * 3, [-1e30] * 3
    for mesh in doc["meshes"]:
        for prim in mesh["primitives"]:
            acc = doc["accessors"][prim["attributes"]["POSITION"]]
            for i in range(3):
                lo[i], hi[i] = min(lo[i], acc["min"][i]), max(hi[i], acc["max"][i])
    return lo, hi


def fit_glb(src, dst, size, yaw_degrees=0.0, bottom=None, uniform=True, auto_length_axis="z"):
    """Writes `src` to `dst` with its root node's matrix set so the model is centred on the origin (its bottom at `bottom` if given) and about `size` metres
    (x, y, z) across: uniformly to the best fit (`uniform`), or stretched to the size on x and z with y following their mean. A model whose long side is along X is
    turned a quarter so the long side runs along `auto_length_axis` (Meshy gives a vehicle in any direction); `yaw_degrees` then turns it further."""
    doc, rest = read_glb(src)
    lo, hi = glb_bounds(doc)
    dims = [hi[i] - lo[i] for i in range(3)]
    centre = [(hi[i] + lo[i]) / 2 for i in range(3)]
    theta = math.radians(yaw_degrees)
    if auto_length_axis == "z" and dims[0] > dims[2] * 1.2:
        theta += math.pi / 2
    c, s = math.cos(theta), math.sin(theta)
    # the model's x and z extents after the turn
    ex = abs(c) * dims[0] + abs(s) * dims[2]
    ez = abs(s) * dims[0] + abs(c) * dims[2]
    if uniform:
        k = min(size[0] / ex, size[1] / dims[1], size[2] / ez)
        sx = sy = sz = k
    else:
        sx, sz = size[0] / ex, size[2] / ez
        sy = (sx + sz) / 2
    # matrix = R * S (column-major), then a translation that puts the (turned, scaled) centre on the origin
    cols = [[sx * c, 0.0, -sx * s], [0.0, sy, 0.0], [sz * s, 0.0, sz * c]]
    mapped = [sum(cols[j][i] * centre[j] for j in range(3)) for i in range(3)]
    t = [-mapped[0], -mapped[1], -mapped[2]]
    if bottom is not None:
        t[1] = bottom - (lo[1] * sy)
    doc["nodes"][0]["matrix"] = [cols[0][0], cols[0][1], cols[0][2], 0.0, cols[1][0], cols[1][1], cols[1][2], 0.0, cols[2][0], cols[2][1], cols[2][2], 0.0, t[0], t[1], t[2], 1.0]
    write_glb_doc(dst, doc, rest)
    return dims


MESHY_FIT = {
    # name in the meshy directory: (the model it replaces, size x y z in metres, options)
    "crate": ("crate", (0.92, 0.92, 0.92), {}),
    "lantern": ("lantern", (0.55, 0.88, 0.55), {}),
    "jeep_body": ("jeep", (1.9, 1.5, 4.2), {"uniform": False, "bottom": -0.34}),
    # Environment props, scattered through the clearing when they are there: (fitted size is the largest each may be).
    "oak_tree": ("oak", (7.0, 8.5, 7.0), {"bottom": 0.0}),
    "grass_clump": ("grass", (0.8, 0.7, 0.8), {"bottom": 0.0}),
    "wildflowers": ("flowers", (0.7, 0.55, 0.7), {"bottom": 0.0}),
    "ruin_arch": ("arch", (5.5, 5.0, 1.6), {"bottom": 0.0}),
}


# --- the world: heights, a site, a frame to place things in ---------------------------------------------------------------------
def read_water(terrain_glb):
    """The baked terrain model's water surfaces, from its accessors' bounds: the sea (the biggest one) and the lakes (every other piece): (x0, z0, x1, z1, y)."""
    doc, _ = read_glb(terrain_glb)
    water = []
    for prim in doc["meshes"][0]["primitives"]:
        if "AETHER_materials_water" in doc["materials"][prim["material"]].get("extensions", {}):
            a = doc["accessors"][prim["attributes"]["POSITION"]]
            water.append((a["min"][0], a["min"][2], a["max"][0], a["max"][2], a["min"][1], a["count"]))
    assert water, "no water in the terrain model"
    sea = max(water, key=lambda w: w[5])
    lakes = [w[:5] for w in water if w is not sea]
    return sea[4], lakes


def choose_pond(ground, lakes, site, reach=130.0):
    """The lake near the clearing that a boat has the most room on: at least 1.5 m deep somewhere and 12 m across, the largest such within `reach` metres.
    Returns its rectangle and level, its middle, and which way is long (+Z or +X as a unit vector in the plane)."""
    best = None
    for (x0, z0, x1, z1, y) in lakes:
        cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
        if math.hypot(cx - site[0], cz - site[1]) > reach or min(x1 - x0, z1 - z0) < 12.0:
            continue
        depth = max(y - ground.height(x, z) for x in range(int(x0) + 2, int(x1) - 1, 3) for z in range(int(z0) + 2, int(z1) - 1, 3))
        if depth < 1.5:
            continue
        area = (x1 - x0) * (z1 - z0)
        if best is None or area > best["area"]:
            best = {"x0": x0, "z0": z0, "x1": x1, "z1": z1, "y": y, "cx": cx, "cz": cz, "area": area, "depth": depth,
                    "dir": (0.0, 1.0) if (z1 - z0) >= (x1 - x0) else (1.0, 0.0), "length": max(x1 - x0, z1 - z0)}
    return best


class Ground:
    """The baked ground's heights, read from the scene's own terrain collider (a regular grid of vertices)."""

    def __init__(self, collider):
        verts = collider["vertices"]
        n = int(round(math.sqrt(len(verts))))
        assert n * n == len(verts), "the terrain collider is not a square grid"
        self.n = n
        self.step = verts[1][0] - verts[0][0]
        self.h = [[verts[z * n + x][1] for x in range(n)] for z in range(n)]

    def height(self, x, z):
        fx, fz = x / self.step, z / self.step
        ix, iz = max(0, min(self.n - 2, int(math.floor(fx)))), max(0, min(self.n - 2, int(math.floor(fz))))
        tx, tz = max(0.0, min(1.0, fx - ix)), max(0.0, min(1.0, fz - iz))
        h = self.h
        return (h[iz][ix] * (1 - tx) * (1 - tz) + h[iz][ix + 1] * tx * (1 - tz) + h[iz + 1][ix] * (1 - tx) * tz + h[iz + 1][ix + 1] * tx * tz)

    def find_site(self, sea, lakes, margin=14.0):
        """The flattest 36 m window a few metres above the sea (so the bay is in view) and clear of every lake by `margin` metres, away from the map's edge."""
        w, best = 13, None
        for iz in range(14, self.n - w - 14):
            for ix in range(14, self.n - w - 14):
                x0, z0 = ix * self.step - margin, iz * self.step - margin
                x1, z1 = (ix + w) * self.step + margin, (iz + w) * self.step + margin
                if any(not (lx1 < x0 or lx0 > x1 or lz1 < z0 or lz0 > z1) for lx0, lz0, lx1, lz1, _ in lakes):
                    continue
                cells = [self.h[iz + j][ix + i] for j in range(w) for i in range(w)]
                lo, hi = min(cells), max(cells)
                mean = sum(cells) / len(cells)
                if lo < sea + 3 or mean > sea + 14:
                    continue
                score = (hi - lo, ix, iz)
                if best is None or score < best[0]:
                    best = (score, ix, iz)
        assert best is not None, "no flat ground above the sea"
        _, ix, iz = best
        return (ix + w / 2) * self.step, (iz + w / 2) * self.step

    def direction_to_water(self, x, z, sea, reach=220.0):
        """The horizontal direction from (x, z) to the mean of the ground that is under the sea, nearby; the slope's downhill if there is none."""
        sx = sz = count = 0
        for iz in range(self.n):
            for ix in range(self.n):
                px, pz = ix * self.step, iz * self.step
                if self.h[iz][ix] < sea and (px - x) ** 2 + (pz - z) ** 2 < reach ** 2:
                    sx += px - x
                    sz += pz - z
                    count += 1
        if count == 0:
            sx = -(self.height(x + 6, z) - self.height(x - 6, z))
            sz = -(self.height(x, z + 6) - self.height(x, z - 6))
        length = math.hypot(sx, sz) or 1.0
        return sx / length, sz / length


class Frame:
    """Positions in the clearing: `a` along the downhill direction (towards the bay), `r` to the right of it, `h` above the ground there."""

    def __init__(self, ground, cx, cz, down):
        self.g, self.cx, self.cz = ground, cx, cz
        self.dx, self.dz = down
        self.rx, self.rz = -self.dz, self.dx  # right of a facing direction (x, z) with up +Y: for facing -Z (0, -1) the right is +X (1, 0)
        # check handedness: facing f = (dx, dz), right = f x up = (-dz, dx)

    def xz(self, a, r):
        return self.cx + a * self.dx + r * self.rx, self.cz + a * self.dz + r * self.rz

    def at(self, a, r, h=0.0):
        x, z = self.xz(a, r)
        return [x, self.g.height(x, z) + h, z]

    def yaw(self):
        """The heading that makes a thing's +Z axis point downhill (a rotation about +Y)."""
        return math.atan2(self.dx, self.dz)


def quat_y(theta):
    return [0.0, math.sin(theta / 2), 0.0, math.cos(theta / 2)]


def pond_point(pond, along, side, height):
    """A point `along` metres from the pond's middle along its long way, `side` metres to one side (left of that way), and `height` above the water."""
    dx, dz = pond["dir"]
    return [pond["cx"] + dx * along - dz * side, pond["y"] + height, pond["cz"] + dz * along + dx * side]


def look(from_pos, to_pos):
    """Yaw and pitch in degrees for the player's camera path: yaw 0 faces -Z and turns left, pitch negative looks down."""
    dx, dy, dz = (to_pos[i] - from_pos[i] for i in range(3))
    return math.degrees(math.atan2(-dx, -dz)), math.degrees(math.atan2(dy, math.hypot(dx, dz)))


# --- the scene ---------------------------------------------------------------------------------------------------------------
def guid_for(name):
    return str(uuid.uuid5(NAMESPACE, name))


class SceneBuilder:
    def __init__(self, scene):
        self.scene = scene

    def add(self, name, components, pos=(0, 0, 0), rot=(0, 0, 0, 1), scale=(1, 1, 1), tags=(), guid=None):
        comps = dict(components)
        comps["EntityName"] = {"$v": 1, "value": name}
        comps["Transform"] = {"$v": 2, "position": [round(v, 4) for v in pos], "rotation": [round(v, 6) for v in rot], "scale": list(scale)}
        if tags:
            comps["Tags"] = {"$v": 1, "names": list(tags)}
        entity = {"components": comps, "guid": guid or guid_for("entity/" + name)}
        self.scene["entities"].append(entity)
        return entity["guid"]


def model(path):
    return {"$v": 1, "asset_path": path, "model": ""}


def box_collider(half, center=(0, 0, 0), friction=0.6):
    return {"$v": 1, "center": list(center), "half_extents": list(half), "friction": friction, "is_trigger": False, "report_stay": False, "restitution": 0.0}


def flammable(flame_height, fuel=40.0):
    """A Flammable that burns in the rain (it takes a very wet thing to put it out) with flames and a light `flame_height` above its middle."""
    return {"$v": 1, "enabled": True, "fuel": fuel, "ignition_heat": 1.0, "spread_radius": 3.5, "heat_rate": 0.8, "cool_rate": 0.25, "wetness": 0.0, "douse_wetness": 0.92,
            "consume": False, "flame_effect": "Fx/Flame.avfx", "flame_offset": [0.0, flame_height, 0.0], "light_color": [1.0, 0.55, 0.2], "light_intensity": 110.0,
            "light_range": 15.0}


def build_scene(src, ground, site, down, sea, props=(), pond=None, start=0.0):
    scene = json.loads(src.read_text())
    ents = scene["entities"]
    frame = Frame(ground, site[0], site[1], down)
    sb = SceneBuilder(scene)

    # The clearing: the plants within 32 m of the middle come out (entity 16 onwards are the plants; before them the ground, the player, camera and lights).
    cx, cz = site
    def in_the_way(e):
        """A plant (an entity with nothing but a model) in the clearing, or on the pond and 14 m round it (so the boat can be filmed)."""
        if not ("ModelRenderer" in e["components"] and len(e["components"]) == 2):
            return False
        x, _, z = e["components"]["Transform"]["position"]
        if (x - cx) ** 2 + (z - cz) ** 2 < 32.0 ** 2:
            return True
        return pond is not None and pond["x0"] - 14 <= x <= pond["x1"] + 14 and pond["z0"] - 14 <= z <= pond["z1"] + 14
    kept = ents[:16] + [e for e in ents[16:] if not in_the_way(e)]
    removed = len(ents) - len(kept)
    scene["entities"] = kept
    ents = kept

    # The hero stands in the clearing, and does not walk (the controller script would also move the camera); the camera gets a film lens.
    player = ents[6]
    player["components"].pop("ScriptComponent", None)
    player["components"]["Transform"]["position"] = frame.at(-10, -3, 0.15)
    player["components"]["Transform"]["rotation"] = quat_y(frame.yaw() + math.pi)  # looks back up the slope, at the car
    camera = ents[12]
    camera["components"]["CineCamera"] = {"$v": 1, "focal_length_mm": 24.0, "sensor_width_mm": 36.0, "sensor_height_mm": 20.25, "aperture_f": 4.0, "focus_distance": 20.0}

    # The picture: one global post-process volume (the weather driver moves its fog and grade).
    sb.add("Post", {"PostProcessVolume": {"$v": 1, "global": True, "weight": 1.0, "bloom_intensity": 0.12, "vignette": 0.2, "saturation": 1.05, "chromatic_aberration": 0.10,
                                           "film_grain": 0.06, "sharpen": 0.18, "screen_space_reflections": 0.5, "auto_exposure": 0.6, "fog_density": 0.0007}})

    # Scripts, rain, the blast's light.
    script = lambda path, props=None: {"$v": 1, "properties": [{"$v": 1, "name": k, "value": str(v)} for k, v in (props or {}).items()],  # noqa: E731
                                       "script": guid_for("asset/" + path)}
    sb.add("Sky", {"ScriptComponent": script("Scripts/WeatherDriver.luau", {"sea_level": round(sea, 2)})})
    sb.add("Director", {"ScriptComponent": script("Scripts/DemoDirector.luau", {"start_time": start} if start else None)})
    for asset in ("RainLight", "RainHeavy"):
        sb.add(asset, {"ParticleSystem": {"$v": 1, "asset": f"Fx/{asset}.avfx", "auto_activate": False}})
    sb.add("BlastLight", {"PointLight": {"$v": 1, "color": [1.0, 0.65, 0.3], "intensity": 0.0, "range": 28.0, "cast_shadows": False}}, tags=("BlastLight",))

    # The environment: oaks round the edge, grass and wildflowers over the ground, a ruined arch for a backdrop. Visual only (no colliders), kept off the
    # car's route (a corridor along the slope), the gallows and the pile.
    import random
    rng = random.Random(7)
    keep_out = [(-2.0, -11.0, 4.5), (2.0, 13.0, 5.0)]

    def usable(a, r, margin=0.0):
        if -26.0 < a < 46.0 and -13.0 < r < 8.0:   # the car's corridor
            return False
        return all((a - ka) ** 2 + (r - kr) ** 2 > (kd + margin) ** 2 for ka, kr, kd in keep_out)

    if "oak" in props:
        for i, (a, r) in enumerate([(-12, -24), (8, -24), (14, -19), (18, 14), (6, 27), (-14, 22), (-26, 12), (-26, -14), (30, 24), (-4, 34)]):
            sb.add(f"Oak{i}", {"ModelRenderer": model("Models/oak.glb")}, pos=frame.at(a, r), rot=quat_y(rng.uniform(0, 6.28)), scale=[rng.uniform(0.85, 1.25)] * 3)
    for name, count in (("grass", 170), ("flowers", 70)):
        if name not in props:
            continue
        placed = 0
        while placed < count:
            a, r = rng.uniform(-30, 36), rng.uniform(-28, 30)
            if not usable(a, r, 0.5):
                continue
            sb.add(f"{name.title()}{placed}", {"ModelRenderer": model(f"Models/{name}.glb")}, pos=frame.at(a, r), rot=quat_y(rng.uniform(0, 6.28)),
                   scale=[rng.uniform(0.8, 1.4)] * 3)
            placed += 1
    if "arch" in props:
        sb.add("Arch", {"ModelRenderer": model("Models/arch.glb")}, pos=frame.at(26, 24), rot=quat_y(frame.yaw() + math.pi / 2), scale=(1, 1, 1))

    # The gallows, with a lantern on a rope in a field of wind.
    gx, gz = frame.xz(-2, -11)
    ground_y = lambda a, r: frame.at(a, r)[1]  # noqa: E731
    beam_h = 3.4
    for r in (-1.7, 1.7):
        p = frame.at(-2, -11 + r)
        sb.add(f"GallowsPost{int(r > 0)}", {"ModelRenderer": model("Models/plank.glb"), "BoxCollider": box_collider((0.5, 0.5, 0.5)), "Flammable": flammable(0.5)},
               pos=(p[0], p[1] + beam_h / 2 - 0.5, p[2]), rot=quat_y(frame.yaw()), scale=(0.26, beam_h + 1.0, 0.26))
    top = max(ground_y(-2, -11 - 1.7), ground_y(-2, -11 + 1.7)) + beam_h
    bp = frame.at(-2, -11)
    sb.add("GallowsBeam", {"ModelRenderer": model("Models/plank.glb"), "BoxCollider": box_collider((0.5, 0.5, 0.5)), "Flammable": flammable(0.3)},
           pos=(bp[0], top, bp[2]), rot=quat_y(frame.yaw()), scale=(0.30, 0.30, 4.0), tags=("Gallows",))
    rope_len = 1.7
    lantern_guid = guid_for("entity/Lantern")
    lantern_y = top - 0.15 - rope_len - 0.42
    sb.add("Rope", {"Rope": {"$v": 1, "enabled": True, "length": rope_len, "segments": 14, "direction": [0.0, -1.0, 0.0], "radius": 0.035, "mass": 1.2, "damping": 0.35,
                              "pin_start": True, "end_entity": lantern_guid, "end_anchor": [0.0, 0.42, 0.0], "segment_model": "Models/rope_segment.glb"}},
           pos=(bp[0], top - 0.15, bp[2]))
    sb.add("Lantern", {"ModelRenderer": model("Models/lantern.glb"), "BoxCollider": box_collider((0.20, 0.38, 0.20)),
                       "RigidBody": {"$v": 1, "motion": "Dynamic", "mass": 3.0, "linear_damping": 0.25, "angular_damping": 0.6},
                       "PointLight": {"$v": 1, "color": [1.0, 0.68, 0.34], "intensity": 26.0, "range": 18.0, "cast_shadows": False}},
           pos=(bp[0], lantern_y, bp[2]), tags=("Lantern",), guid=lantern_guid)
    sb.add("WindField", {"ForceField": {"$v": 1, "enabled": True, "shape": "Box", "half_extents": [7.0, 6.0, 7.0], "mode": "Directional", "strength": 0.0,
                                         "direction": [0.4, 0.0, 0.9], "mass_independent": True}}, pos=(bp[0], top - 1.5, bp[2]))

    # A pond with a boat on it (the Water kit): the surface is the rendered water's own level and rectangle, the boat a Floater with a propeller and a rudder.
    if pond is not None:
        dx, dz = pond["dir"]
        hx, hz = (pond["x1"] - pond["x0"]) / 2 - 0.5, (pond["z1"] - pond["z0"]) / 2 - 0.5
        sb.add("Pond", {"WaterSurface": {"$v": 1, "enabled": True, "infinite": False, "half_extents": [hx, 0.0, hz], "density": 1000.0, "wave_height": 0.10,
                                         "wave_length": 7.0, "wave_speed": 1.6, "wave_direction": 35.0, "current_speed": 0.0, "current_direction": 0.0}},
               pos=(pond["cx"], pond["y"], pond["cz"]), tags=("Pond",))
        start = pond_point(pond, -pond["length"] / 2 + 9.0, 0.0, 0.0)
        sb.add("Boat", {"ModelRenderer": model("Models/boat.glb"), "BoxCollider": box_collider((0.95, 0.3, 2.2)),
                        "RigidBody": {"$v": 1, "motion": "Dynamic", "mass": 700.0, "linear_damping": 0.0, "angular_damping": 0.05},
                        "Floater": {"$v": 1, "enabled": True, "half_extents": [0.95, 0.3, 2.2], "offset": [0.0, 0.0, 0.0], "columns_x": 3, "columns_z": 5,
                                    "buoyancy_scale": 1.0, "linear_drag": 3.0, "angular_drag": 1.5},
                        "Boat": {"$v": 1, "enabled": True, "thrust": 14000.0, "reverse_ratio": 0.4, "turn_torque": 2500.0, "turn_speed": 3.0, "keel_drag": 6.0,
                                 "max_speed": 7.0, "propeller_offset": [0.0, -0.7, -2.2]}},   # the outboard's propeller hangs below the hull, so a wave does not lift it clear
               pos=(start[0], pond["y"] + 0.35, start[2]), rot=quat_y(math.atan2(dx, dz)), tags=("Boat",))

    # The crates on a pallet, a pyramid of 4-3-2-1 two deep, each one breakable.
    px, pz = frame.xz(2, 13)
    base = max(ground_y(2 + da, 13 + dr) for da in (-1.6, 1.6) for dr in (-2.8, 2.8)) + 0.04
    sb.add("Pallet", {"ModelRenderer": model("Models/pallet.glb"), "BoxCollider": box_collider((0.5, 0.5, 0.5))},
           pos=(px, base - 0.25, pz), rot=quat_y(frame.yaw()), scale=(3.4, 0.5, 5.2))
    count = 0
    for k in range(4):
        for i in range(4 - k):
            for depth in (-0.5, 0.5):
                a, r = 2 + depth, 13 + (i - (4 - k - 1) / 2) * 0.96
                p = frame.at(a, r)
                crate = {"ModelRenderer": model("Models/crate.glb"), "BoxCollider": box_collider((0.45, 0.45, 0.45), friction=0.7),
                         "RigidBody": {"$v": 1, "motion": "Dynamic", "mass": 18.0, "linear_damping": 0.05, "angular_damping": 0.1},
                         "Destructible": {"$v": 1, "enabled": True, "health": 0.0, "break_speed": 3.5, "pieces_x": 2, "pieces_y": 2, "pieces_z": 2, "mass": 18.0,
                                          "burst_speed": 3.0, "inherit_velocity": 0.7, "piece_lifetime": 30.0}}
                sb.add(f"Crate{count}", crate, pos=(p[0], base + 0.46 + k * 0.93, p[2]), rot=quat_y(frame.yaw()), tags=("Crate",))
                count += 1

    # The jeep and its wheels (children of the body, turned each frame by the director from what the Vehicles kit reports).
    car_guid = guid_for("entity/Jeep")
    cp = frame.at(-22, 1.0)
    car_y = cp[1] + 0.4 + 0.30 + 0.05
    sb.add("Jeep", {"ModelRenderer": model("Models/jeep.glb"), "BoxCollider": box_collider((0.95, 0.4, 2.1)),
                    "RigidBody": {"$v": 1, "motion": "Dynamic", "mass": 1400.0, "linear_damping": 0.0, "angular_damping": 0.05},
                    "Vehicle": {"$v": 1, "enabled": True, "drive": "AllWheel", "wheel_radius": 0.35, "wheel_width": 0.25, "engine_torque": 650.0, "max_steer_degrees": 32.0}},
           pos=(cp[0], car_y, cp[2]), rot=quat_y(frame.yaw()), tags=("Jeep",), guid=car_guid)
    for i, (x, z) in enumerate(((-0.855, 1.47), (0.855, 1.47), (-0.855, -1.47), (0.855, -1.47))):
        sb.add(f"Wheel{i}", {"ModelRenderer": model("Models/wheel.glb"), "Parent": {"$v": 1, "parent": car_guid}}, pos=(x, -0.36, z), tags=(f"Wheel{i}",))

    return scene, frame, removed, count


# --- the camera path -----------------------------------------------------------------------------------------------------------
# (second, x, z) of the jeep while it drives, from the director's log for the clearing (380, 388): the film's clock is the game's clock, so it repeats.
CAR_TRACK = [(26, (401.1, 384.5)), (27, (399.5, 384.6)), (28, (394.8, 383.9)), (29, (386.7, 381.1)), (30, (374.7, 378.9)), (31, (362.1, 384.5)),
             (32, (351.4, 394.4)), (33, (340.2, 403.9))]


def camera_path(frame, pond=None):
    """Shots as (time, camera position, look-at target); the path is resampled twice a second, the position kept above the ground, and each sample aimed at its target."""
    G = frame.at(-2, -11, 2.0)       # the lantern on its gallows
    P = frame.at(2, 13, 0.9)         # the pile
    shots = [(0.0, frame.at(-150, 55, 78), frame.at(0, 0, 4)), (6.0, frame.at(-85, 30, 32), frame.at(-5, -4, 3))]
    for t, phi in ((12.0, 200), (16.0, 250), (20.0, 300)):
        a = -2 + 8.0 * math.cos(math.radians(phi))
        r = -11 + 8.0 * math.sin(math.radians(phi))
        shots.append((t, frame.at(a, r, 1.9), G))
    shots += [(22.0, frame.at(-8.0, 9.5, 1.6), P), (25.0, frame.at(-4.5, 10.5, 1.4), P)]
    # The car, once a second, as the director's log reported it (the run is deterministic). Two fixed cameras beside its path pan to follow it, with a cut between.
    spots = [(391.0, 370.0), (354.0, 372.0)]
    for t, (x, z) in CAR_TRACK:
        cx_, cz_ = spots[0] if t < 31 else spots[1]
        shots.append((float(t), [cx_, frame.g.height(cx_, cz_) + 1.7, cz_], [x, frame.g.height(x, z) + 0.8, z], t in (CAR_TRACK[0][0], 31)))
    G2 = frame.at(-2, -11, 2.8)      # the gallows' beam, where the lightning strikes
    shots += [(34.0, frame.at(-9.5, 13.0, 1.0), P, True), (36.9, frame.at(-8.2, 13.0, 1.1), P),
              (37.0, frame.at(-13.0, -4.0, 2.2), G2, True), (40.9, frame.at(-10.0, -5.5, 2.4), G2)]
    if pond is not None:
        # The boat starts nine metres in from the pond's near end and drives the long way, 6 m/s at its fastest (from the director's log: it is 23, 13, 1 and 10 m
        # from the middle at 41, 44, 46 and 48 s). The camera trails it down the pond, off to one side, and at the end lifts away over it.
        half = pond["length"] / 2
        shots += [(41.0, pond_point(pond, -half + 5.0, 4.0, 2.4), pond_point(pond, -half + 14.0, 0.0, 0.4), True),
                  (44.0, pond_point(pond, -half + 7.0, 6.0, 2.4), pond_point(pond, -half + 23.0, 0.0, 0.4)),
                  (46.5, pond_point(pond, -half + 22.0, 7.0, 2.8), pond_point(pond, -half + 40.0, 0.0, 0.4)),
                  (48.0, pond_point(pond, -half + 12.0, 14.0, 12.0), pond_point(pond, 6.0, 0.0, 0.0))]
    else:
        shots += [(41.0, frame.at(-34.0, 18.0, 10.0), frame.at(40, 0, 8), True), (48.0, frame.at(-65.0, 32.0, 24.0), frame.at(90, 0, 12))]
    keys = []
    times = sorted({round(0.5 * i, 3) for i in range(int(shots[-1][0] * 2) + 1)} | {round(sh[0] - 0.04, 3) for sh in shots if len(sh) > 3 and sh[3]})
    for t in times:
        k = max(i for i, sh in enumerate(shots) if sh[0] <= t) if t < shots[-1][0] else len(shots) - 2
        (t0, p0, g0), (t1, p1, g1) = shots[k][:3], shots[k + 1][:3]
        u = (t - t0) / (t1 - t0)
        if len(shots[k + 1]) > 3 and shots[k + 1][3]:
            u = 0.0   # a cut: hold this shot until the next one starts
        u = u * u * (3 - 2 * u) if 0.0 <= u <= 1.0 else u
        pos = [p0[i] + (p1[i] - p0[i]) * u for i in range(3)]
        tgt = [g0[i] + (g1[i] - g0[i]) * u for i in range(3)]
        pos[1] = max(pos[1], frame.g.height(pos[0], pos[2]) + 1.0)   # never under the hill
        yaw, pitch = look(pos, tgt)
        keys.append({"t": round(t, 3), "position": [round(v, 3) for v in pos], "yaw": round(yaw, 2), "pitch": round(pitch, 2)})
    return {"fps": FPS, "keys": keys}


# --- the pipeline --------------------------------------------------------------------------------------------------------------
def link_tree(src, dst):
    """Copies a tree with hard links where it can (the bake is hundreds of megabytes)."""
    for path in src.rglob("*"):
        target = dst / path.relative_to(src)
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(path, target)
            except OSError:
                shutil.copy2(path, target)


def asset_meta(content, rel, importer):
    meta = {"$type": "AssetMeta", "$v": 1, "guid": guid_for("asset/" + rel), "importer": importer, "importer_version": 1, "labels": [], "settings": {}, "source_hash": ""}
    (content / (rel + ".ameta")).write_text(json.dumps(meta, indent=2) + "\n")


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=str(REPO / "build"), help="the build tree (aether_cook, aether_player, aether_tests)")
    parser.add_argument("--work", required=True, help="where the project, the pak, the frames and the video go")
    parser.add_argument("--bake-dir", default=None, help="a content root the opt-in bake has already written (default: bake into WORK/bake)")
    parser.add_argument("--size", default="1280x720")
    parser.add_argument("--encode-only", action="store_true", help="skip filming and encode the frames already in WORK/frames")
    parser.add_argument("--out", default=None, help="the mp4 (default: WORK/woodland_demo.mp4)")
    parser.add_argument("--meshy-dir", default=None, help="glbs from meshy_assets.py (crate, lantern, jeep_body) to use in place of the placeholder models")
    parser.add_argument("--jeep-yaw", type=float, default=0.0, help="turns the Meshy jeep about Y (180 if it came out facing backwards)")
    parser.add_argument("--no-render", action="store_true", help="stop after cooking (and writing the camera path)")
    parser.add_argument("--no-titles", action="store_true")
    parser.add_argument("--start", type=float, default=0.0, help="film only from this second on (a preview of the end: the weather cues catch up at once)")
    parser.add_argument("--virtual-geometry", action="store_true", help="film with the player's virtual geometry (clusters on the terrain tiles and larger models)")
    args = parser.parse_args()
    build = pathlib.Path(args.build)
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    width, height = (int(v) for v in args.size.split("x"))

    # 1. The bake.
    bake = pathlib.Path(args.bake_dir) if args.bake_dir else work / "bake"
    if not (bake / "Generated" / "Woodland" / "Woodland.ascene").exists():
        env = dict(os.environ, AETHER_PROCEDURAL_BAKE_DIR=str(bake), AETHER_TEST_FILTER="OptInBake")
        bake.mkdir(parents=True, exist_ok=True)
        cmd = [str(build / "tests" / "aether_tests")]
        if shutil.which("xvfb-run"):
            cmd = ["xvfb-run", "-a"] + cmd
        run(cmd, env=env)

    # 2. The project: the Woodland's content, our scripts, effects and models.
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    content = project / "Content"
    for sub in ("Generated/Woodland", "Scripts", "Input"):
        link_tree(bake / sub, content / sub)
    for sub in ("Scripts", "Fx"):
        (content / sub).mkdir(parents=True, exist_ok=True)
        for f in (SAMPLE / "Content" / sub).iterdir():
            shutil.copy2(f, content / sub / f.name)
            asset_meta(content, f"{sub}/{f.name}", "Script" if f.suffix == ".luau" else "ParticleSystem")
    (content / "Models").mkdir(exist_ok=True)
    for name, build_model in MODELS.items():
        mm.write_glb(build_model(), content / "Models" / f"{name}.glb")
        asset_meta(content, f"Models/{name}.glb", "Model")
    props = set()
    if args.meshy_dir:
        for source, (target, size, options) in MESHY_FIT.items():
            glb = pathlib.Path(args.meshy_dir) / f"{source}.glb"
            if glb.exists():
                options = dict(options)
                if source == "jeep_body":
                    options["yaw_degrees"] = args.jeep_yaw
                dims = fit_glb(glb, content / "Models" / f"{target}.glb", size, **options)
                props.add(target)
                print(f"meshy {source}: {dims[0]:.2f} x {dims[1]:.2f} x {dims[2]:.2f} -> Models/{target}.glb", flush=True)

    # 3. The scene.
    src = bake / "Generated" / "Woodland" / "Woodland.ascene"
    base_scene = json.loads(src.read_text())
    ground = Ground(base_scene["entities"][1]["components"]["MeshCollider"])
    sea, lakes = read_water(bake / "Generated" / "Woodland" / "Woodland_terrain.glb")
    site = ground.find_site(sea, lakes)
    down = ground.direction_to_water(site[0], site[1], sea)
    pond = choose_pond(ground, lakes, site)
    print("pond:", "none" if pond is None else f"{pond['x1'] - pond['x0']:.0f} x {pond['z1'] - pond['z0']:.0f} m at level {pond['y']:.1f}, {pond['depth']:.1f} m deep, middle ({pond['cx']:.0f}, {pond['cz']:.0f})", flush=True)
    scene, frame, removed, crates = build_scene(src, ground, site, down, sea, props, pond, args.start)
    (content / "Scenes").mkdir(exist_ok=True)
    (content / "Scenes" / "WoodlandDemo.ascene").write_text(json.dumps(scene, separators=(",", ":")))
    asset_meta(content, "Scenes/WoodlandDemo.ascene", "Scene")
    print(f"site ({site[0]:.0f}, {site[1]:.0f}) at {ground.height(*site):.1f} m, downhill {down[0]:.2f},{down[1]:.2f}; {removed} plants cleared; {crates} crates", flush=True)

    # 4. The project file and the cook.
    proj = json.loads((REPO / "samples" / "forest_valley" / "ForestValley.aproject").read_text())
    proj["name"] = "WoodlandDemo"
    proj["startup_scene"] = "Scenes/WoodlandDemo.ascene"
    proj["always_cook"] = ["Scenes/WoodlandDemo.ascene", "Fx/", "Models/", "Scripts/"]
    (project / "WoodlandDemo.aproject").write_text(json.dumps(proj, indent=2))
    paks = work / "paks"
    if paks.exists():
        shutil.rmtree(paks)
    run([str(build / "tools" / "cook" / "aether_cook"), str(project / "WoodlandDemo.aproject"), "--out", str(paks), "--config", "development"], cwd=project)

    path = camera_path(frame, pond)
    if args.start:   # a preview of the end of the film: the director's clock starts there (the weather's cues catch up at once) and the camera path is cut to match
        path["keys"] = [dict(k, t=round(k["t"] - args.start, 3)) for k in path["keys"] if k["t"] >= args.start]
        path["keys"][0]["t"] = 0.0
    (work / "camera_path.json").write_text(json.dumps(path, indent=1))
    if args.no_render:
        return

    frames = work / "frames"
    if not args.encode_only:
        # 5. Film it, converting each frame to a JPEG as soon as it is complete (the BMPs are huge).
        frames = work / "frames"
        if frames.exists():
            shutil.rmtree(frames)
        frames.mkdir()
        from PIL import Image
        done = {"stop": False, "converted": 0}

        def converter():
            while True:
                bmps = sorted(frames.glob("frame_*.bmp"))
                ready = bmps if done["stop"] else bmps[:-1]  # the newest may still be being written
                for bmp in ready:
                    Image.open(bmp).convert("RGB").save(bmp.with_suffix(".jpg"), quality=93)
                    bmp.unlink()
                    done["converted"] += 1
                if done["stop"] and not list(frames.glob("frame_*.bmp")):
                    return
                time.sleep(0.5)

        thread = threading.Thread(target=converter, daemon=True)
        thread.start()
        env = dict(os.environ, VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/lvp_icd.json")
        if args.virtual_geometry:
            env.update(AETHER_VIRTUAL_GEOMETRY="1", AETHER_VIRTUAL_GEOMETRY_MIN_TRIANGLES="4000", AETHER_VIRTUAL_GEOMETRY_PIXELS="1")
        cmd = [str(build / "player" / "aether_player"), "--pak", str(paks), "--scene", "Scenes/WoodlandDemo.ascene", "--backend", "vulkan", "--size", f"{width}x{height}",
               "--camera-path", str(work / "camera_path.json"), "--record-dir", str(frames)]
        if shutil.which("xvfb-run"):
            cmd = ["xvfb-run", "-a"] + cmd
        started = time.time()
        print("+", " ".join(cmd), flush=True)
        with open(work / "player.log", "w") as log:
            result = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
        done["stop"] = True
        thread.join()
        print(f"filmed {done['converted']} frames in {time.time() - started:.0f} s (player exit {result.returncode}); log: {work / 'player.log'}", flush=True)
        if result.returncode != 0 and done["converted"] < int(DURATION * FPS):
            sys.exit(result.returncode)
        if result.returncode != 0:
            print("warning: the player reported errors (see the log) but filmed every frame", flush=True)

    # 6. Encode, with title cards.
    out = pathlib.Path(args.out) if args.out else work / "woodland_demo.mp4"
    font = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    cards = [(0.5, 4.0, "AETHER ENGINE", 54), (6.5, 11.0, "Weather + Time of Day", 40), (12.5, 19.5, "Ropes + Force Fields", 40), (26.0, 32.5, "Vehicles", 40),
             (34.2, 36.8, "Destruction", 40), (37.4, 40.8, "Fire", 40), (41.4, 44.6, "Water + Boats", 40), (45.0, 47.6, "The Woodland, running on the engine", 38)]
    vf = []
    if not args.no_titles and pathlib.Path(font).exists():
        for t0, t1, text, size in cards:
            vf.append(f"drawtext=fontfile={font}:text='{text}':fontcolor=white:fontsize={int(size * height / 720)}:x=(w-text_w)/2:y=h*0.84:alpha='if(lt(t,{t0}),0,if(lt(t,{t0 + 0.5}),(t-{t0})/0.5,if(lt(t,{t1 - 0.5}),1,if(lt(t,{t1}),({t1}-t)/0.5,0))))':shadowcolor=black@0.7:shadowx=2:shadowy=2")
    vf.append(f"fade=t=in:st=0:d=0.8,fade=t=out:st={DURATION - 1.2}:d=1.2")
    run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-framerate", str(FPS), "-i", str(frames / "frame_%05d.jpg"), "-vf", ",".join(vf), "-c:v", "libx264", "-crf", "17",
         "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(out)])
    print(f"wrote {out} ({out.stat().st_size / 1e6:.1f} MB)", flush=True)


if __name__ == "__main__":
    main()
