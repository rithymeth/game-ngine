"""Ashen Frontier's terrain as one function, height(x, z), and the files made from it: the visual mesh (Models/Terrain.glb), its painted colour
texture (Textures/TerrainColour.png) and the collision grid (a HeightfieldCollider, written into the OpenWorld scene by tools/world_scene.py).

Everything (props, the map, the collision, the picture) reads the same function, so the ground a model stands on is the ground the character walks on.

The shape: layered mesas and canyon gullies in the Fallen Basin and Iron Wastes, soft hills in the Verdant Array, a raised plateau with one long ramp
in the Hollow Core, ridges between the regions with a natural pass through each, an irregular rim of mountains round the walkable world, and flat
ground cleared at every point of interest (so a building is not on a slope). Wavelengths are 20 m and up, so a 4 m collision grid and a 2 m render grid
describe it the same way to within a few centimetres.

    python tools/terrain.py            # writes Content/Models/Terrain.glb, Content/Textures/TerrainColour.png and Content/Data/terrain_collision.json
"""

import json
import math
import pathlib
import struct

import numpy as np

import world

ROOT = pathlib.Path(__file__).resolve().parent.parent
HALF = world.HALF + 10.0   # the terrain's half-size: a little past the world's edge
RENDER_CELL = 2.0
COLLISION_CELL = 4.0
TEXTURE_SIZE = 1024


def smoothstep(a, b, x):
    t = np.clip((np.asarray(x, dtype=np.float64) - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


_LATTICES = {}


def vnoise(x, z, seed):
    """Smooth value noise in [0, 1], one unit per lattice cell."""
    if seed not in _LATTICES:
        _LATTICES[seed] = np.random.RandomState(seed).rand(256, 256)
    lat = _LATTICES[seed]
    x, z = np.asarray(x, dtype=np.float64), np.asarray(z, dtype=np.float64)
    xi, zi = np.floor(x).astype(np.int64), np.floor(z).astype(np.int64)
    fx, fz = x - xi, z - zi
    u, v = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    a, b = lat[zi & 255, xi & 255], lat[zi & 255, (xi + 1) & 255]
    c, d = lat[(zi + 1) & 255, xi & 255], lat[(zi + 1) & 255, (xi + 1) & 255]
    return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v


def fbm(x, z, seed, wavelength, octaves=4, gain=0.5):
    """Fractal noise in about [-1, 1]; `wavelength` is the biggest feature in metres."""
    total, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        total = total + amp * (vnoise(x / wavelength * 2 ** o, z / wavelength * 2 ** o, seed + o * 17) * 2 - 1)
        norm += amp
        amp *= gain
    return total / norm


def _region_base(xw, zw):
    sx, sz = smoothstep(-1, 1, xw / 22.0), smoothstep(-1, 1, zw / 22.0)   # 0 west / north .. 1 east / south
    basin, iron, verdant, core = 0.0, 4.0, -1.5, 0.0                        # the plateau is added separately
    north = verdant * (1 - sx) + core * sx
    south = basin * (1 - sx) + iron * sx
    return north * (1 - sz) + south * sz


def _poi_flats():
    flats = [(0.0, 0.0, 34.0), (world.SPAWN["position"][0], world.SPAWN["position"][2], 22.0)]
    big = {"landmark": 26.0, "dungeon": 28.0, "faction": 22.0}
    for pid, region, _name, kind, _off in world.POIS:
        x, z = world.poi_position(pid)
        flats.append((x, z, big.get(kind, 18.0)))
    return flats


_FLATS = _poi_flats()
_CORE = world.REGIONS["hollow_core"]["centre"]


def _raw_height(x, z):
    x, z = np.asarray(x, dtype=np.float64), np.asarray(z, dtype=np.float64)
    xw = x + 26.0 * fbm(x, z, 101, 95, 2)
    zw = z + 26.0 * fbm(x, z, 202, 95, 2)
    r_from_centre = np.hypot(x, z)
    h = _region_base(xw, zw)

    # layered mesas: noise cut into terraces with soft risers, strongest in the Basin and the Iron Wastes (south side)
    mesa_weight = smoothstep(-30, 60, z) * (0.65 + 0.45 * smoothstep(-40, 40, -x)) * 1.1 + 0.22
    t = (fbm(x, z, 303, 110, 3) * 0.5 + 0.5) * 3.4
    f = t - np.floor(t)
    terraced = (np.floor(t) + smoothstep(0.78, 1.0, f)) * 3.8
    h = h + (terraced - 4.0) * mesa_weight * smoothstep(20, 70, r_from_centre)

    # canyon gullies: ridged noise cut into the ground, mostly in the Basin
    ridge = 1 - np.abs(fbm(x, z, 404, 80, 3))
    gully = np.clip(ridge, 0, 1) ** 5 * 12.0
    h = h - gully * smoothstep(-20, 70, z) * smoothstep(40, -40, x) * smoothstep(25, 60, r_from_centre)

    # soft hills in the Verdant Array
    verdant_w = smoothstep(20, -60, z) * smoothstep(40, -40, x)
    h = h + fbm(x, z, 505, 70, 3) * 3.2 * verdant_w

    # ridges between the regions along the warped axes, with a pass through each at the middle of its edge, and the hub open
    for d_axis, along, gate in ((np.abs(xw), z, 115.0), (np.abs(zw), x, 115.0)):
        profile = np.exp(-(d_axis / 17.0) ** 2) * 15.0 * (0.8 + 0.4 * (fbm(x, z, 606, 40, 2) * 0.5 + 0.5))
        through = np.exp(-((np.abs(along) - gate) / 24.0) ** 2)                 # the pass
        hub_open = smoothstep(26, 62, r_from_centre)
        h = h + profile * (1 - through) * hub_open

    # the Hollow Core plateau, with one long ramp on its south side
    cx, cz = _CORE
    dx, dz = xw - cx, zw - cz
    d = np.hypot(dx, dz)
    south = np.exp(-(np.arctan2(dx, dz) / 0.42) ** 2)                            # 0 away from the ramp, 1 right of it (south = +z)
    edge = 7.0 + 36.0 * south
    plateau = 7.0 * (1 - smoothstep(80 - edge, 80 + edge * 0.6, d))
    h = h + plateau + (fbm(x, z, 707, 50, 2) * 0.35 * plateau / 7.0)

    # the rim: mountains round an irregular walkable outline
    theta = np.arctan2(z, x)
    outline = 208.0 + 26.0 * fbm(3.0 * np.cos(theta) + 20, 3.0 * np.sin(theta) + 20, 808, 1.0, 3)
    outside = r_from_centre - outline
    rim = smoothstep(-6, 40, outside) * (58.0 + 18.0 * fbm(x, z, 909, 45, 3))
    h = h + np.maximum(rim, 0)
    return h


_FLAT_BASE = [float(_raw_height(fx, fz)) for fx, fz, _r in _FLATS]   # the natural height at each flat's centre, worked out once


def height(x, z):
    """The ground's height at (x, z) in metres (arrays or scalars)."""
    x, z = np.asarray(x, dtype=np.float64), np.asarray(z, dtype=np.float64)
    h = _raw_height(x, z)
    for (fx, fz, radius), base in zip(_FLATS, _FLAT_BASE):                      # flat ground where the game puts things
        w = smoothstep(radius * 1.5, radius * 0.7, np.hypot(x - fx, z - fz))
        h = h * (1 - w) + base * w
    return h


def slope(x, z, step=1.5):
    """The steepness (rise over run) at (x, z)."""
    gx = (height(x + step, z) - height(x - step, z)) / (2 * step)
    gz = (height(x, z + step) - height(x, z - step)) / (2 * step)
    return np.hypot(gx, gz)


def region_weights(x, z):
    """How much each region (basin, iron, verdant, core) owns (x, z): a smooth, warped four-way split, for painting."""
    xw = x + 26.0 * fbm(x, z, 101, 95, 2)
    zw = z + 26.0 * fbm(x, z, 202, 95, 2)
    sx, sz = smoothstep(-1, 1, xw / 18.0), smoothstep(-1, 1, zw / 18.0)
    return {"iron": sx * sz, "basin": (1 - sx) * sz, "verdant": (1 - sx) * (1 - sz), "core": sx * (1 - sz)}


# --- the colour texture ------------------------------------------------------------------------------------------------------------
def paint(size=TEXTURE_SIZE):
    coords = (np.arange(size) + 0.5) / size * (2 * HALF) - HALF
    x, z = np.meshgrid(coords, coords)            # rows are z (down the image), columns are x
    h, s = height(x, z), slope(x, z)
    w = region_weights(x, z)
    fine = fbm(x, z, 11, 6, 3)
    mid = fbm(x, z, 12, 28, 3)
    bands = 0.5 + 0.5 * np.sin(h * 1.45 + mid * 2.4)                                # strata: layers of rock read as bands in the cliff faces
    rock_amount = smoothstep(0.45, 0.95, s)
    tone = lambda r, g, b: np.stack([np.full_like(h, r), np.full_like(h, g), np.full_like(h, b)], axis=-1)
    sand_a, sand_b = tone(0.72, 0.42, 0.24), tone(0.84, 0.60, 0.36)                  # oxidised red and ochre
    rock_a, rock_b = tone(0.50, 0.24, 0.15), tone(0.72, 0.42, 0.25)
    basin = sand_a * (0.55 + 0.45 * (0.5 + 0.5 * mid))[..., None] + sand_b * (0.25 * (0.5 + 0.5 * fine))[..., None]
    iron = tone(0.38, 0.33, 0.30) * (0.8 + 0.3 * fine)[..., None] + tone(0.35, 0.16, 0.08) * (0.4 * smoothstep(0.2, 0.9, mid * 0.5 + 0.5))[..., None]
    verdant = tone(0.27, 0.37, 0.16) * (0.75 + 0.35 * mid)[..., None] + tone(0.40, 0.32, 0.18) * (0.30 * smoothstep(0.1, 0.8, fine * 0.5 + 0.5))[..., None]
    core = tone(0.16, 0.16, 0.20) * (0.8 + 0.4 * fine)[..., None] + tone(0.10, 0.20, 0.32) * (0.25 * smoothstep(0.3, 0.9, mid * 0.5 + 0.5))[..., None]
    ground = basin * w["basin"][..., None] + iron * w["iron"][..., None] + verdant * w["verdant"][..., None] + core * w["core"][..., None]
    rock = rock_a * (1 - bands)[..., None] + rock_b * bands[..., None]
    rock = rock * (0.75 + 0.3 * fine)[..., None]
    rock = rock * (1 - 0.45 * w["verdant"][..., None]) + tone(0.24, 0.30, 0.16) * (0.45 * w["verdant"][..., None])   # moss on the rock in the Array
    rock = rock * (1 - 0.7 * w["core"][..., None]) + tone(0.13, 0.13, 0.17) * (0.7 * w["core"][..., None])
    colour = ground * (1 - rock_amount[..., None]) + rock * rock_amount[..., None]
    dust = smoothstep(0.0, 0.2, 0.25 - s) * 0.10                                           # a lighter dust on the flats
    colour = colour * (1 - dust[..., None]) + tone(0.80, 0.62, 0.45) * dust[..., None]
    # the cleared ground at each point of interest: a pale, packed floor
    for fx, fz, radius in _FLATS:
        near = smoothstep(radius * 0.95, radius * 0.55, np.hypot(x - fx, z - fz)) * 0.28
        colour = colour * (1 - near[..., None]) + tone(0.50, 0.42, 0.36) * near[..., None]
    return np.clip(colour, 0, 1)


def write_png(path, rgb):
    import zlib
    img = (np.clip(rgb, 0, 1) ** (1 / 1.0) * 255 + 0.5).astype(np.uint8)
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# --- the mesh and the collision grid -------------------------------------------------------------------------------------------------
def write_mesh(path):
    n = int(round(2 * HALF / RENDER_CELL)) + 1
    axis = np.linspace(-HALF, HALF, n)
    x, z = np.meshgrid(axis, axis)
    h = height(x, z)
    gx = np.gradient(h, RENDER_CELL, axis=1)
    gz = np.gradient(h, RENDER_CELL, axis=0)
    normals = np.stack([-gx, np.ones_like(h), -gz], axis=-1)
    normals /= np.linalg.norm(normals, axis=-1, keepdims=True)
    positions = np.stack([x, h, z], axis=-1).reshape(-1, 3).astype("<f4")
    nrm = normals.reshape(-1, 3).astype("<f4")
    uv = np.stack([(x + HALF) / (2 * HALF), (z + HALF) / (2 * HALF)], axis=-1).reshape(-1, 2).astype("<f4")
    idx = np.arange(n * n, dtype=np.uint32).reshape(n, n)
    v00, v10, v01, v11 = idx[:-1, :-1], idx[:-1, 1:], idx[1:, :-1], idx[1:, 1:]
    tris = np.stack([np.stack([v00, v01, v10], -1), np.stack([v10, v01, v11], -1)], axis=2).reshape(-1, 3).astype("<u4")
    blobs, views, accessors = bytearray(), [], []

    def view(data, target):
        views.append({"buffer": 0, "byteOffset": len(blobs), "byteLength": len(data), "target": target})
        blobs.extend(data)
        while len(blobs) % 4:
            blobs.append(0)
        return len(views) - 1

    pv, nv, uvv, iv = view(positions.tobytes(), 34962), view(nrm.tobytes(), 34962), view(uv.tobytes(), 34962), view(tris.tobytes(), 34963)
    accessors = [
        {"bufferView": pv, "componentType": 5126, "count": int(len(positions)), "type": "VEC3", "min": positions.min(0).tolist(), "max": positions.max(0).tolist()},
        {"bufferView": nv, "componentType": 5126, "count": int(len(nrm)), "type": "VEC3"},
        {"bufferView": uvv, "componentType": 5126, "count": int(len(uv)), "type": "VEC2"},
        {"bufferView": iv, "componentType": 5125, "count": int(tris.size), "type": "SCALAR"},
    ]
    doc = {"asset": {"version": "2.0", "generator": "Ashen Frontier terrain.py"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "Terrain"}],
           "meshes": [{"name": "Terrain", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
           "materials": [{"name": "terrain", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 0.95}}],
           "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(blobs)}]}
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    total = 12 + 8 + len(text) + 8 + len(blobs)
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(text), b"JSON") + text + struct.pack("<I4s", len(blobs), b"BIN\0") + bytes(blobs))
    return n, int(tris.shape[0])


def collision_grid():
    """The HeightfieldCollider's data: an even sample count, the cell size and the local position of sample (0, 0)."""
    samples = int(round(2 * (HALF - COLLISION_CELL) / COLLISION_CELL))
    samples -= samples % 2
    origin = -COLLISION_CELL * (samples - 1) / 2
    axis = origin + COLLISION_CELL * np.arange(samples)
    x, z = np.meshgrid(axis, axis)
    h = height(x, z)
    return {"samples": samples, "cell_size": COLLISION_CELL, "origin": [origin, 0.0, origin], "heights": [round(float(v), 3) for v in h.reshape(-1)]}


def main():
    (ROOT / "Content" / "Models").mkdir(parents=True, exist_ok=True)
    (ROOT / "Content" / "Textures").mkdir(parents=True, exist_ok=True)
    (ROOT / "Content" / "Data").mkdir(parents=True, exist_ok=True)
    verts, tris = write_mesh(ROOT / "Content" / "Models" / "Terrain.glb")
    print(f"Terrain.glb: {verts} x {verts} vertices, {tris} triangles")
    write_png(ROOT / "Content" / "Textures" / "TerrainColour.png", paint())
    print("TerrainColour.png:", TEXTURE_SIZE, "x", TEXTURE_SIZE)
    grid = collision_grid()
    (ROOT / "Content" / "Data" / "terrain_collision.json").write_text(json.dumps(grid, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
    print("collision grid:", grid["samples"], "x", grid["samples"], "at", grid["cell_size"], "m")
    h = np.array(grid["heights"]).reshape(grid["samples"], grid["samples"])
    print(f"height range {h.min():.1f} .. {h.max():.1f} m")


if __name__ == "__main__":
    main()
