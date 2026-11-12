#!/usr/bin/env python3
"""Generates the low-poly placeholder models of Ashen Frontier as binary glTF (.glb) files in Content/Models.

Every model is built from a few primitives (boxes, cylinders, cones, wedges, rocks), flat shaded, with a handful of
materials (colour factors, no textures), so a model is a few hundred triangles and the whole set is re-made by running
this script. They are stand-ins: the point is silhouettes that read at distance and a consistent palette (sun-bleached
hull, oxidised red, dark metal, cobalt machine lights, safety yellow), to be replaced by real art later without touching
the scenes (a replacement keeps the file name).

    python tools/make_models.py            # writes Content/Models/*.glb
    python tools/make_models.py --list     # names, triangle counts and sizes
"""

import argparse
import json
import math
import pathlib
import random
import struct

OUT = pathlib.Path(__file__).resolve().parent.parent / "Content" / "Models"

# --- palette: name -> (base colour rgb, metallic, roughness, emissive rgb or None) ---------------------------------------------
PALETTE = {
    "hull":    ((0.74, 0.72, 0.66), 0.35, 0.70, None),   # sun-bleached industrial paint
    "weathered": ((0.14, 0.12, 0.11), 0.15, 0.85, None),  # a wreck's scratched, riveted plating
    "rust":    ((0.50, 0.20, 0.11), 0.20, 0.85, None),   # oxidised red
    "dark":    ((0.14, 0.15, 0.17), 0.60, 0.55, None),   # dark metal
    "steel":   ((0.45, 0.47, 0.50), 0.70, 0.45, None),
    "cobalt":  ((0.05, 0.15, 0.45), 0.10, 0.40, (0.10, 0.40, 1.00)),  # machine lights
    "amber":   ((0.55, 0.30, 0.05), 0.10, 0.40, (1.00, 0.55, 0.10)),  # hazard / warning lights
    "yellow":  ((0.85, 0.65, 0.08), 0.10, 0.60, None),   # safety yellow
    "suit":    ((0.80, 0.38, 0.14), 0.05, 0.75, None),   # Kael's flight suit
    "visor":   ((0.04, 0.08, 0.12), 0.80, 0.15, (0.05, 0.20, 0.35)),
    "sand":    ((0.62, 0.40, 0.25), 0.00, 0.95, None),   # basin ground and rock
    "stone":   ((0.42, 0.28, 0.20), 0.00, 0.95, None),
    "green":   ((0.18, 0.42, 0.16), 0.00, 0.80, None),   # rare vegetation
}


# --- tiny vector maths -----------------------------------------------------------------------------------------------------
def rot_matrix(rx, ry, rz):
    """Rotation by degrees about X, then Y, then Z."""
    ax, ay, az = (math.radians(a) for a in (rx, ry, rz))
    cx, sx, cy, sy, cz, sz = math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay), math.cos(az), math.sin(az)
    mx = ((1, 0, 0), (0, cx, -sx), (0, sx, cx))
    my = ((cy, 0, sy), (0, 1, 0), (-sy, 0, cy))
    mz = ((cz, -sz, 0), (sz, cz, 0), (0, 0, 1))

    def mul(a, b):
        return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))

    return mul(mz, mul(my, mx))


def apply(m, p):
    return tuple(sum(m[i][k] * p[k] for k in range(3)) for i in range(3))


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


class Model:
    """Triangles grouped by material. Each primitive is placed with a centre and a rotation, and its triangles are wound
    to face away from the primitive's centre (every primitive here is convex), so the model needs no hand-checked winding."""

    def __init__(self):
        self.tris = {}

    def _add(self, material, vertices, faces, centre, rot):
        m = rot_matrix(*rot)
        world = [tuple(c + o for c, o in zip(apply(m, v), centre)) for v in vertices]
        local_centre = tuple(sum(w[i] for w in world) / len(world) for i in range(3))
        out = self.tris.setdefault(material, [])
        for face in faces:  # fan-triangulate each face
            for k in range(1, len(face) - 1):
                a, b, c = world[face[0]], world[face[k]], world[face[k + 1]]
                n = cross(sub(b, a), sub(c, a))
                mid = tuple((a[i] + b[i] + c[i]) / 3 for i in range(3))
                if dot(n, sub(mid, local_centre)) < 0:
                    b, c = c, b
                out.append((a, b, c))

    def box(self, material, centre, size, rot=(0, 0, 0)):
        sx, sy, sz = (s / 2 for s in size)
        v = [(-sx, -sy, -sz), (sx, -sy, -sz), (sx, sy, -sz), (-sx, sy, -sz), (-sx, -sy, sz), (sx, -sy, sz), (sx, sy, sz), (-sx, sy, sz)]
        f = [(0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (3, 2, 6, 7), (0, 3, 7, 4), (1, 2, 6, 5)]
        self._add(material, v, f, centre, rot)

    def cylinder(self, material, centre, radius, height, sides=8, top_radius=None, rot=(0, 0, 0)):
        """An upright cylinder (a cone when top_radius is 0, a frustum when it is smaller); rotate it to lay it down."""
        top = radius if top_radius is None else top_radius
        h = height / 2
        v = [(radius * math.cos(2 * math.pi * i / sides), -h, radius * math.sin(2 * math.pi * i / sides)) for i in range(sides)]
        if top > 0:
            v += [(top * math.cos(2 * math.pi * i / sides), h, top * math.sin(2 * math.pi * i / sides)) for i in range(sides)]
        else:
            v += [(0, h, 0)]
        f = [tuple(range(sides))]  # the bottom
        if top > 0:
            f.append(tuple(range(sides, 2 * sides)))
            for i in range(sides):
                j = (i + 1) % sides
                f.append((i, j, sides + j, sides + i))
        else:
            for i in range(sides):
                f.append((i, (i + 1) % sides, sides))
        self._add(material, v, f, centre, rot)

    def wedge(self, material, centre, size, rot=(0, 0, 0)):
        """A ramp: a box with its top sloping down along +z (a nose, a roof, a ramp)."""
        sx, sy, sz = (s / 2 for s in size)
        v = [(-sx, -sy, -sz), (sx, -sy, -sz), (sx, sy, -sz), (-sx, sy, -sz), (-sx, -sy, sz), (sx, -sy, sz)]
        f = [(0, 1, 2, 3), (0, 1, 5, 4), (3, 2, 5, 4), (0, 3, 4), (1, 2, 5)]
        self._add(material, v, f, centre, rot)

    def rock(self, material, centre, radius, seed=1, squash=0.7, rot=(0, 0, 0)):
        """A lumpy icosahedron: twenty triangles, each corner pushed in or out a little."""
        rng = random.Random(seed)
        t = (1 + 5 ** 0.5) / 2
        base = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t), (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
        v = []
        for p in base:
            n = math.sqrt(dot(p, p))
            k = radius * rng.uniform(0.78, 1.15) / n
            v.append((p[0] * k, p[1] * k * squash, p[2] * k))
        f = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
             (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
        self._add(material, v, f, centre, rot)

    def merge(self, other, rot=(0, 0, 0), shift=(0, 0, 0), pivot=(0, 0, 0)):
        """Adds another model's triangles, turned about `pivot` and then moved by `shift` (a wreck pitched into the ground)."""
        m = rot_matrix(*rot)
        for material, tris in other.tris.items():
            out = self.tris.setdefault(material, [])
            for tri in tris:
                out.append(tuple(tuple(c + d for c, d in zip(apply(m, sub(p, pivot)), tuple(pivot[i] + shift[i] for i in range(3)))) for p in tri))

    def count(self):
        return sum(len(t) for t in self.tris.values())

    def bounds(self):
        pts = [p for tris in self.tris.values() for tri in tris for p in tri]
        return tuple(min(p[i] for p in pts) for i in range(3)), tuple(max(p[i] for p in pts) for i in range(3))


# --- the models ----------------------------------------------------------------------------------------------------------------
def kael():
    """The player: a salvage pilot in an orange flight suit, 1.8 m."""
    m = Model()
    m.box("dark", (-0.11, 0.42, 0), (0.17, 0.84, 0.20))    # legs
    m.box("dark", (0.11, 0.42, 0), (0.17, 0.84, 0.20))
    m.box("suit", (0, 1.13, 0), (0.46, 0.62, 0.28))        # torso
    m.box("dark", (0, 1.02, 0.0), (0.48, 0.10, 0.30))      # belt
    m.box("suit", (-0.32, 1.13, 0), (0.14, 0.58, 0.16))    # arms
    m.box("suit", (0.32, 1.13, 0), (0.14, 0.58, 0.16))
    m.box("steel", (0, 1.15, -0.22), (0.34, 0.46, 0.16))   # backpack
    m.box("cobalt", (0, 1.20, -0.31), (0.20, 0.06, 0.02))  # pack light
    m.box("hull", (0, 1.60, 0), (0.30, 0.30, 0.30))        # head
    m.box("visor", (0, 1.62, 0.13), (0.26, 0.12, 0.06))    # visor
    return m


def mule3():
    """MULE-3: the first machine the player rebuilds. A low four-legged cargo hauler, 1.0 m high, 1.6 m long."""
    m = Model()
    m.box("hull", (0, 0.62, 0), (0.80, 0.34, 1.30))        # body
    m.box("rust", (0, 0.82, -0.18), (0.72, 0.10, 0.80))    # cargo deck
    m.box("dark", (0, 0.40, 0), (0.60, 0.16, 1.00))        # belly
    for x in (-0.38, 0.38):
        for z in (-0.52, 0.52):
            m.box("dark", (x, 0.22, z), (0.14, 0.44, 0.14))        # legs
            m.box("steel", (x, 0.04, z), (0.22, 0.08, 0.26))       # feet
    m.box("hull", (0, 0.78, 0.60), (0.50, 0.30, 0.26))     # sensor head
    m.box("cobalt", (0, 0.80, 0.74), (0.30, 0.08, 0.04))   # eye bar
    m.box("yellow", (0, 0.88, -0.62), (0.70, 0.04, 0.05))  # hazard stripe
    return m


def spark2():
    """SPARK-2: a small hovering scanner drone, 0.5 m."""
    m = Model()
    m.rock("steel", (0, 0.35, 0), 0.22, seed=7, squash=0.8)  # body
    m.cylinder("dark", (0, 0.35, 0), 0.30, 0.04, sides=10)   # rotor ring
    m.box("cobalt", (0, 0.35, 0.20), (0.14, 0.08, 0.04))     # eye
    m.box("hull", (0.0, 0.52, 0), (0.04, 0.16, 0.04))        # antenna
    m.box("amber", (0.0, 0.62, 0), (0.07, 0.04, 0.07))
    return m


def sentinel4():
    """SENTINEL-4: a defence turret on three legs, 2.4 m; the shield dome is a separate material for later."""
    m = Model()
    for i in range(3):
        a = i * 120
        x, z = 0.7 * math.sin(math.radians(a)), 0.7 * math.cos(math.radians(a))
        m.box("dark", (x * 0.6, 0.9, z * 0.6), (0.14, 1.8, 0.14), rot=(z * 14, 0, -x * 14))   # legs splayed outward
        m.box("steel", (x, 0.05, z), (0.30, 0.10, 0.30))
    m.cylinder("hull", (0, 2.0, 0), 0.50, 0.50, sides=8, top_radius=0.40)   # body
    m.box("dark", (0, 2.0, 0.55), (0.16, 0.16, 0.70))                       # barrel
    m.box("cobalt", (0, 2.18, 0.30), (0.34, 0.07, 0.07))                    # sensor band
    m.box("rust", (0, 2.35, 0), (0.60, 0.08, 0.60))                         # cap
    return m


def drone():
    """An industrial drone: the common enemy, 0.9 m, hovering."""
    m = Model()
    m.rock("rust", (0, 0.9, 0), 0.30, seed=3, squash=0.7)
    m.box("dark", (0, 0.9, 0), (1.0, 0.05, 0.12))
    m.box("dark", (0, 0.9, 0), (0.12, 0.05, 1.0))
    for x, z in ((-0.5, 0), (0.5, 0), (0, -0.5), (0, 0.5)):
        m.cylinder("steel", (x, 0.93, z), 0.16, 0.03, sides=8)
    m.box("amber", (0, 0.88, 0.28), (0.18, 0.10, 0.06))
    return m


def freighter():
    """The crashed freighter that is the landmark of the Fallen Basin: about 64 m long, broken in two, the forward hull pitched nose-down into the sand."""
    fwd = Model()
    fwd.box("weathered", (0, 5.5, 8), (12, 8, 26))                       # forward hull
    fwd.wedge("weathered", (0, 5.5, 25.5), (12, 8, 9))                   # nose
    fwd.box("rust", (0, 9.8, 6), (11.2, 0.6, 22))                   # roof plating, oxidised
    fwd.box("dark", (0, 2.2, 8), (13, 1.2, 24))                     # keel
    for z in (-2, 6, 14, 20):
        fwd.box("steel", (0, 5.5, z), (12.4, 8.4, 0.5))             # frames
    fwd.box("cobalt", (-6.05, 6.5, 24), (0.15, 1.2, 4.5))           # cockpit window glow
    fwd.box("cobalt", (6.05, 6.5, 24), (0.15, 1.2, 4.5))
    fwd.box("rust", (-9, 5, -2), (0.4, 7, 12), rot=(0, 20, 12))     # a torn panel
    fwd.box("yellow", (0, 1.0, 19), (13.2, 0.35, 0.5))              # hazard stripe
    fwd.box("steel", (8.5, 8.5, 12), (0.5, 4.5, 9), rot=(0, 0, -25))  # a wing stub, snapped
    m = Model()
    m.merge(fwd, rot=(9, 0, 0), shift=(0, -1.5, 6), pivot=(0, 0, 8))  # nose down, sunk
    aft = Model()
    aft.box("weathered", (3, 6, -22), (10, 7, 16), rot=(0, 0, 8))        # the aft section, broken off and tilted
    aft.box("dark", (3, 9.6, -23), (9, 0.5, 14), rot=(0, 0, 8))
    for x in (-3.5, 3.5, 9):
        aft.cylinder("dark", (x, 4, -32), 1.6, 3.0, sides=8, rot=(90, 0, 0))   # engine bells
        aft.cylinder("amber", (x, 4, -33.6), 1.0, 0.4, sides=8, rot=(90, 0, 0))
    aft.box("rust", (3, 11, -20), (3, 3, 4), rot=(10, 30, 20))      # wreckage on the break
    m.merge(aft, rot=(0, 12, 0), shift=(-2, -0.4, -4), pivot=(3, 0, -22))
    return m


def scrap_pile():
    m = Model()
    m.rock("rust", (0, 0.5, 0), 1.1, seed=11, squash=0.55)
    m.box("steel", (0.8, 0.55, 0.3), (1.4, 0.18, 0.7), rot=(0, 25, 18))
    m.box("hull", (-0.7, 0.8, -0.4), (0.8, 0.12, 1.2), rot=(14, -30, -22))
    m.box("dark", (0.1, 0.9, 0.7), (0.30, 0.7, 0.30), rot=(20, 0, 25))
    m.cylinder("steel", (-0.3, 0.4, 0.9), 0.22, 0.9, sides=6, rot=(0, 0, 80))
    m.box("cobalt", (0.4, 1.02, -0.2), (0.14, 0.10, 0.14))        # a live component to scan
    return m


def salvage_crate():
    m = Model()
    m.box("steel", (0, 0.4, 0), (0.9, 0.8, 0.6))
    m.box("yellow", (0, 0.4, 0.305), (0.9, 0.12, 0.02))
    m.box("dark", (0, 0.82, 0), (0.95, 0.06, 0.65))
    m.box("cobalt", (0.3, 0.55, 0.31), (0.12, 0.08, 0.02))
    return m


def rock_a():
    m = Model()
    m.rock("stone", (0, 0.9, 0), 1.6, seed=21, squash=0.85)
    m.rock("sand", (1.5, 0.45, 0.6), 0.8, seed=22, squash=0.7)
    return m


def rock_b():
    m = Model()
    m.rock("stone", (0, 1.4, 0), 2.3, seed=31, squash=1.1)
    m.rock("stone", (-2.2, 0.6, 1.0), 1.1, seed=32, squash=0.8)
    m.rock("sand", (1.8, 0.4, -1.4), 0.9, seed=33, squash=0.6)
    return m


def cliff():
    """A blocky cliff section for the basin's walls and the gullies, 12 x 8 x 6."""
    m = Model()
    m.box("stone", (0, 3.0, 0), (12, 6, 6))
    m.box("sand", (-2, 6.4, -0.6), (7, 1.6, 4.5), rot=(0, 8, 0))
    m.box("stone", (4.5, 5.2, 0.8), (4, 4.5, 4), rot=(0, -12, 0))
    m.box("rust", (0, 1.5, 3.05), (11, 0.6, 0.2))                 # an oxide band
    return m


def pylon():
    """A power pylon with a cobalt line light: part of the outpost's grid."""
    m = Model()
    m.cylinder("steel", (0, 2.0, 0), 0.22, 4.0, sides=6, top_radius=0.14)
    m.box("dark", (0, 4.1, 0), (1.4, 0.14, 0.14))
    m.box("dark", (0, 3.6, 0), (1.0, 0.10, 0.10))
    m.box("cobalt", (0, 4.3, 0), (0.16, 0.16, 0.16))
    m.box("steel", (0, 0.1, 0), (0.7, 0.2, 0.7))
    return m


def generator():
    """The outpost's first power generator, 1.6 x 1.4 x 1.2."""
    m = Model()
    m.box("hull", (0, 0.55, 0), (1.5, 1.0, 1.1))
    m.box("dark", (0, 0.08, 0), (1.7, 0.16, 1.3))
    m.cylinder("steel", (0.45, 1.25, 0), 0.22, 0.6, sides=8)      # exhaust
    m.box("rust", (-0.35, 1.08, 0.2), (0.6, 0.16, 0.5))
    m.box("cobalt", (0, 0.7, 0.56), (0.9, 0.10, 0.02))            # status strip
    m.box("amber", (-0.55, 0.35, 0.56), (0.12, 0.12, 0.02))
    return m


def fab_bench():
    """A fabrication bench: the one crafting station of the first sprint."""
    m = Model()
    m.box("steel", (0, 0.45, 0), (1.8, 0.1, 0.9))                 # top
    for x in (-0.8, 0.8):
        for z in (-0.38, 0.38):
            m.box("dark", (x, 0.2, z), (0.10, 0.4, 0.10))
    m.box("hull", (-0.5, 0.75, -0.2), (0.6, 0.5, 0.4))            # fabricator unit
    m.box("cobalt", (-0.5, 0.82, 0.01), (0.4, 0.2, 0.02))         # its screen
    m.box("dark", (0.45, 0.62, -0.15), (0.12, 0.24, 0.12), rot=(0, 0, 25))   # arm
    m.box("yellow", (0.45, 0.5, 0.25), (0.5, 0.05, 0.3))
    return m


def relay_node():
    """A distress relay: a mast on a base with a dish, 5 m; the objective of the first mission chain."""
    m = Model()
    m.box("steel", (0, 0.25, 0), (1.6, 0.5, 1.6))
    m.cylinder("hull", (0, 2.5, 0), 0.22, 4.0, sides=6, top_radius=0.15)
    m.cylinder("hull", (0.4, 4.4, 0), 0.9, 0.12, sides=10, rot=(0, 0, 25))   # dish
    m.box("cobalt", (0, 4.7, 0), (0.18, 0.18, 0.18))
    m.box("amber", (0.5, 0.7, 0.81), (0.2, 0.2, 0.02))
    return m


def ruin_wall():
    """A broken homestead wall, 5 x 2.6 x 0.4."""
    m = Model()
    m.box("sand", (0, 1.0, 0), (5, 2.0, 0.4))
    m.box("sand", (-1.4, 2.2, 0), (2.0, 0.8, 0.4), rot=(0, 0, 6))
    m.box("rust", (1.5, 1.2, 0.22), (1.2, 1.8, 0.05))
    return m


def outpost_plate():
    """A flat pad marking the outpost footprint, 8 x 0.2 x 8, with cobalt corner lights."""
    m = Model()
    m.box("steel", (0, 0.1, 0), (8, 0.2, 8))
    for x in (-3.8, 3.8):
        for z in (-3.8, 3.8):
            m.box("cobalt", (x, 0.22, z), (0.3, 0.06, 0.3))
    m.box("yellow", (0, 0.205, 3.2), (6, 0.01, 0.2))
    return m


def hazard_node():
    """An electrical-storm hazard emitter: a pylon that arcs. Amber so it reads as a warning, not a friend."""
    m = Model()
    m.cylinder("dark", (0, 1.5, 0), 0.30, 3.0, sides=6, top_radius=0.20)
    m.cylinder("steel", (0, 3.3, 0), 0.6, 0.30, sides=8)
    m.box("amber", (0, 3.6, 0), (0.30, 0.30, 0.30))
    for a in (0, 90, 180, 270):
        m.box("amber", (0.8 * math.sin(math.radians(a)), 3.3, 0.8 * math.cos(math.radians(a))), (0.10, 0.10, 0.5), rot=(0, a, 0))
    return m


def smelter_stack():
    """An Iron Wastes smelter chimney, 18 m, glowing at the top."""
    m = Model()
    m.cylinder("dark", (0, 1.0, 0), 4.2, 2.0, sides=8)                          # base
    m.cylinder("rust", (0, 9.5, 0), 2.6, 17.0, sides=8, top_radius=1.9)         # stack
    m.cylinder("steel", (0, 18.2, 0), 2.1, 0.8, sides=8)                        # rim
    m.box("amber", (0, 18.5, 0), (2.6, 0.3, 2.6))                               # the glow
    m.box("yellow", (0, 3.0, 2.55), (1.2, 0.5, 0.1))
    m.box("steel", (3.4, 4.0, 0), (0.5, 8.0, 0.5), rot=(0, 0, -8))              # a pipe run
    return m


def gantry():
    """A portal gantry, 14 m wide and 9 m high: legs, a beam and a trolley."""
    m = Model()
    for x in (-6.5, 6.5):
        m.box("steel", (x, 4.5, 0), (0.9, 9.0, 0.9))
        m.box("dark", (x, 0.2, 0), (1.8, 0.4, 1.8))
    m.box("rust", (0, 9.0, 0), (14.5, 1.0, 1.4))
    m.box("yellow", (0, 9.55, 0), (14.5, 0.1, 0.4))
    m.box("dark", (2.0, 7.6, 0), (1.6, 1.4, 1.2))                               # trolley
    m.box("steel", (2.0, 5.2, 0), (0.12, 3.4, 0.12))                            # cable
    m.box("amber", (-5.5, 9.7, 0), (0.4, 0.3, 0.4))
    return m


def rail_car():
    """A rail yard car, 3 x 2.6 x 9 m."""
    m = Model()
    m.box("rust", (0, 1.9, 0), (2.9, 1.8, 8.4))
    m.box("dark", (0, 0.8, 0), (2.6, 0.6, 9.0))
    for z in (-3.0, 3.0):
        m.box("steel", (-1.0, 0.35, z), (0.3, 0.7, 1.4))
        m.box("steel", (1.0, 0.35, z), (0.3, 0.7, 1.4))
    m.box("yellow", (0, 2.9, 0), (2.9, 0.12, 8.4))
    return m


def assembly_hall():
    """A buried assembly line's entrance hall: a long low building, 16 x 7 x 28 m, with a doorway glowing cobalt."""
    m = Model()
    m.box("hull", (0, 3.0, 0), (16, 6, 28))
    m.box("dark", (0, 6.2, 0), (16.6, 0.8, 28.6))
    m.box("rust", (0, 3.0, 14.05), (16.1, 6.1, 0.2))
    m.box("cobalt", (0, 2.2, 14.2), (4.0, 4.4, 0.2))                            # the doorway
    for z in (-9, -3, 3, 9):
        m.box("steel", (8.05, 3.5, z), (0.2, 3.0, 1.6))                         # side vents
    m.cylinder("steel", (-5, 7.6, -6), 1.2, 3.0, sides=6)
    return m


def greenhouse():
    """A Verdant Array greenhouse, 12 x 6 x 20 m: a frame, a glassy cobalt-green roof and plants inside."""
    m = Model()
    m.box("steel", (0, 0.4, 0), (12, 0.8, 20))
    m.box("green", (0, 1.6, 0), (10.5, 1.4, 18.5))                               # the beds
    m.box("cobalt", (-2.85, 4.35, 0), (6.4, 0.22, 19.6), rot=(0, 0, 24))         # the two halves of the glassy roof, meeting at a ridge
    m.box("cobalt", (2.85, 4.35, 0), (6.4, 0.22, 19.6), rot=(0, 0, -24))
    m.box("steel", (0, 5.2, 0), (0.3, 0.3, 19.8))                                # ridge beam
    for z in (-9, -3, 3, 9):
        m.box("steel", (-5.9, 2.6, z), (0.25, 4.2, 0.25))
        m.box("steel", (5.9, 2.6, z), (0.25, 4.2, 0.25))
    return m


def dome():
    """A living dome, 18 m across and 8 m high: a ring of panels around a green core."""
    m = Model()
    m.cylinder("steel", (0, 0.4, 0), 9.0, 0.8, sides=12)
    m.cylinder("cobalt", (0, 4.0, 0), 8.6, 6.4, sides=12, top_radius=4.0)
    m.cylinder("cobalt", (0, 7.6, 0), 4.0, 1.6, sides=12, top_radius=0.6)
    m.cylinder("green", (0, 1.6, 0), 6.0, 1.6, sides=10, top_radius=3.2)
    m.box("amber", (0, 8.6, 0), (0.5, 0.5, 0.5))
    return m


def tree_low():
    """The rare saturated vegetation: a low-poly tree, 5 m."""
    m = Model()
    m.cylinder("stone", (0, 1.2, 0), 0.30, 2.4, sides=5, top_radius=0.22)
    m.cylinder("green", (0, 3.2, 0), 1.7, 2.4, sides=6, top_radius=0.0)
    m.cylinder("green", (0, 4.3, 0), 1.2, 2.0, sides=6, top_radius=0.0)
    return m


def reactor_tower():
    """The Hollow Core's reactor column, 30 m: a cobalt-lit cylinder in a dark cage."""
    m = Model()
    m.cylinder("dark", (0, 1.0, 0), 9.0, 2.0, sides=12)
    m.cylinder("steel", (0, 15.0, 0), 4.4, 26.0, sides=10, top_radius=3.8)
    m.cylinder("cobalt", (0, 15.0, 0), 4.6, 12.0, sides=10, top_radius=4.0)
    for i in range(4):
        a = i * 90 + 45
        m.box("dark", (6.4 * math.sin(math.radians(a)), 11.0, 6.4 * math.cos(math.radians(a))), (0.8, 22.0, 0.8))
    m.cylinder("amber", (0, 29.0, 0), 3.2, 1.2, sides=10)
    return m


def ramp():
    """A 10 x 3 x 18 m ramp: down into the Hollow Core's crater."""
    m = Model()
    m.wedge("steel", (0, 1.5, 0), (10, 3, 18), rot=(0, 180, 0))
    m.box("yellow", (-4.8, 3.0, 0), (0.3, 0.2, 18), rot=(10, 0, 0))
    return m


def mesa():
    """A layered sandstone butte, about 16 m across and 13 m high: stacked, staggered slabs of red, ochre and stone with a stepped, blocky outline, like the
    canyon walls of the reference (a few different ones come from the scene giving it a scale and a turn)."""
    m = Model()
    rng = random.Random(5)
    y, w, d = 0.0, 14.0, 11.0
    layers = (("rust", 2.4), ("sand", 1.7), ("stone", 2.6), ("rust", 1.5), ("sand", 2.2), ("stone", 1.4), ("rust", 1.2))
    for material, height in layers:
        m.box(material, (rng.uniform(-0.7, 0.7), y + height / 2, rng.uniform(-0.6, 0.6)), (w, height, d), rot=(0, rng.uniform(-9, 9), 0))
        if rng.random() < 0.7:   # a lower shelf or a fallen block against the face
            m.box("sand" if material != "sand" else "rust", (rng.choice((-1, 1)) * (w / 2 + 0.6), y + height * 0.3, rng.uniform(-2, 2)), (2.6, height * 0.6, 4.2), rot=(0, rng.uniform(-20, 20), 0))
        y += height
        w *= rng.uniform(0.80, 0.92)
        d *= rng.uniform(0.80, 0.92)
    m.box("sand", (0, y + 0.25, 0), (w * 0.9, 0.5, d * 0.9))
    return m


def spire():
    """A thin wind-cut spire, 3 m across and 15 m high, in strata."""
    m = Model()
    y, radius = 0.0, 1.7
    for i, (material, height) in enumerate((("stone", 3.2), ("rust", 2.6), ("sand", 2.8), ("rust", 2.4), ("stone", 2.2), ("sand", 1.8))):
        top = radius * 0.78
        m.cylinder(material, (0.12 * i, y + height / 2, -0.1 * i), radius, height, sides=7, top_radius=top, rot=(0, i * 23, 0))
        y += height
        radius = top
    return m


def arch():
    """A natural rock arch, 13 m across and 9 m high: two strata pillars and a lintel."""
    m = Model()
    for side in (-1, 1):
        y, radius = 0.0, 2.3
        for i, (material, height) in enumerate((("rust", 2.4), ("sand", 2.0), ("stone", 2.2), ("rust", 1.6))):
            top = radius * 0.9
            m.cylinder(material, (side * 5.6 + 0.2 * i * side, y + height / 2, 0), radius, height, sides=7, top_radius=top, rot=(0, i * 31, 0))
            y += height
            radius = top
    m.box("sand", (0, 8.6, 0), (13.0, 1.6, 3.0), rot=(0, 0, 1.5))
    m.box("rust", (0, 9.6, 0), (11.0, 0.8, 2.6), rot=(0, 0, 1.5))
    return m


def ground():
    """A unit ground slab (1 x 0.2 x 1 m, top at y = 0): scaled to make the basin floor."""
    m = Model()
    m.box("sand", (0, -0.1, 0), (1, 0.2, 1))
    return m


def scrub():
    """Dry basin scrub, the only plant: a low cluster of cones."""
    m = Model()
    for i, (x, z, h) in enumerate(((0, 0, 0.7), (0.3, 0.2, 0.5), (-0.3, 0.15, 0.55), (0.05, -0.3, 0.45))):
        m.cylinder("green" if i % 2 == 0 else "stone", (x, h / 2, z), 0.16, h, sides=5, top_radius=0.0)
    return m


MODELS = {
    "Kael": kael, "MULE3": mule3, "SPARK2": spark2, "SENTINEL4": sentinel4, "Drone": drone,
    "Freighter": freighter, "ScrapPile": scrap_pile, "SalvageCrate": salvage_crate,
    "RockA": rock_a, "RockB": rock_b, "Cliff": cliff, "Pylon": pylon, "Generator": generator,
    "FabBench": fab_bench, "RelayNode": relay_node, "RuinWall": ruin_wall, "OutpostPlate": outpost_plate,
    "HazardNode": hazard_node, "Scrub": scrub, "Ground": ground,
    "Mesa": mesa, "Spire": spire, "Arch": arch,
    "SmelterStack": smelter_stack, "Gantry": gantry, "RailCar": rail_car, "AssemblyHall": assembly_hall,
    "Greenhouse": greenhouse, "Dome": dome, "TreeLow": tree_low, "ReactorTower": reactor_tower, "Ramp": ramp,
}


# --- glTF ---------------------------------------------------------------------------------------------------------------------
def write_glb(model, path):
    materials, meshes_prims = [], []
    binary = bytearray()
    views, accessors = [], []

    def add_view(data, target):
        while len(binary) % 4:
            binary.append(0)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data), "target": target})
        binary.extend(data)
        return len(views) - 1

    for name, tris in model.tris.items():
        color, metallic, roughness, emissive = PALETTE[name]
        material = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*color, 1.0], "metallicFactor": metallic, "roughnessFactor": roughness},
                    "doubleSided": False}
        if emissive:
            material["emissiveFactor"] = list(emissive)
        materials.append(material)
        positions, normals, uvs, indices = [], [], [], []
        for a, b, c in tris:
            n = cross(sub(b, a), sub(c, a))
            length = math.sqrt(dot(n, n)) or 1.0
            n = tuple(x / length for x in n)
            for p in (a, b, c):
                indices.append(len(positions))
                positions.append(p)
                normals.append(n)
                uvs.append((p[0] + p[2], p[1] + p[2] * 0.5))   # a planar mapping: a texture later has something to stretch over
        pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        nor_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
        idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
        pv, nv, uvv = add_view(pos_bytes, 34962), add_view(nor_bytes, 34962), add_view(uv_bytes, 34962)
        iv = add_view(idx_bytes, 34963)
        lo = [min(p[i] for p in positions) for i in range(3)]
        hi = [max(p[i] for p in positions) for i in range(3)]
        base = len(accessors)
        accessors += [
            {"bufferView": pv, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": lo, "max": hi},
            {"bufferView": nv, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": uvv, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
            {"bufferView": iv, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
        ]
        meshes_prims.append({"attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2}, "indices": base + 3, "material": len(materials) - 1})
    doc = {
        "asset": {"version": "2.0", "generator": "Ashen Frontier make_models.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": path.stem}],
        "meshes": [{"name": path.stem, "primitives": meshes_prims}],
        "materials": materials, "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(binary)}],
        "extensionsUsed": ["KHR_materials_emissive_strength"],
    }
    for material in doc["materials"]:
        if "emissiveFactor" in material:
            material["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}}  # a light reads as a light after tone mapping
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    while len(binary) % 4:
        binary.append(0)
    total = 12 + 8 + len(text) + 8 + len(binary)
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(text), b"JSON") + text + struct.pack("<I4s", len(binary), b"BIN\0") + bytes(binary))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print names, triangle counts and sizes instead of writing")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    total = 0
    for name, build in MODELS.items():
        model = build()
        lo, hi = model.bounds()
        size = tuple(round(h - l, 2) for l, h in zip(lo, hi))
        total += model.count()
        print(f"{name:14s} {model.count():4d} triangles  {size[0]} x {size[1]} x {size[2]} m")
        if not args.list:
            write_glb(model, OUT / f"{name}.glb")
    print(f"{len(MODELS)} models, {total} triangles")


if __name__ == "__main__":
    main()
