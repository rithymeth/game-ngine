#!/usr/bin/env python3
"""Writes Forest Valley's project file and scenes.

    python3 samples/forest_valley/tools/make_scenes.py

The scenes hold one ProceduralTerrain (the numbers come from forest_valley.cpp's ValleyTerrainComponent: a test regenerates the ground from the
component and compares it to the world the areas and route were built on), a sun, a sky, distance haze and a camera. The cameras are placed from
the areas BuildWorld picks (see the table at the end of the run).
"""
import json
import math
import os
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NS = uuid.UUID("5a1e0f6c-0000-4000-8000-00000000f0e5")


def guid(name):
    return str(uuid.uuid5(NS, name))


def quat(yaw_deg, pitch_deg):
    """Yaw about +Y (0 faces -Z, positive turns left), then pitch about the camera's own x (negative looks down)."""
    y, p = math.radians(yaw_deg) / 2, math.radians(pitch_deg) / 2
    qy = (0.0, math.sin(y), 0.0, math.cos(y))
    qx = (math.sin(p), 0.0, 0.0, math.cos(p))
    x1, y1, z1, w1 = qy
    x2, y2, z2, w2 = qx
    return [w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
            w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2]


def look(frm, to):
    dx, dy, dz = to[0] - frm[0], to[1] - frm[1], to[2] - frm[2]
    flat = math.hypot(dx, dz)
    return math.degrees(math.atan2(-dx / flat, -dz / flat)), math.degrees(math.atan2(dy, flat))


def transform(pos, rot=(0.0, 0.0, 0.0, 1.0)):
    return {"$v": 2, "position": list(pos), "rotation": list(rot), "scale": [1, 1, 1]}


def entity(name, components):
    components = dict(components)
    components["EntityName"] = {"$v": 1, "value": name}
    return {"components": components, "guid": guid(name)}


def scene(camera_from, camera_to):
    yaw, pitch = look(camera_from, camera_to)
    sun_yaw, sun_pitch = look((0, 0, 0), (0.35, -0.55, 0.75))  # the way the light travels: low, from the south-west
    return {
        "$type": "Scene",
        "$version": 1,
        "entities": [
            entity("Sky", {"SkyLight": {"$v": 1, "color": [0.75, 0.82, 0.95], "intensity": 1.0, "cloud_coverage": 0.35}, "Transform": transform((0, 0, 0))}),
            entity("Sun", {"DirectionalLight": {"$v": 1, "cast_shadows": True, "color": [1.0, 0.93, 0.82], "intensity": 3.0},
                           "Transform": transform((0, 0, 0), quat(sun_yaw, sun_pitch))}),
            entity("Valley", {"ProceduralTerrain": {"$v": 1, "visible": True, "seed": 20260101, "samples": 513, "cell_size": 1.0, "height_scale": 50.0,
                                                   "frequency": 2.0, "ridge_amount": 0.3, "sea_level": 0.04, "river_min_catchment": 1500,
                                                   "hydraulic_droplets": 60000},
                              "Transform": transform((0, 0, 0))}),
            entity("Haze", {"PostProcessVolume": {"$v": 1, "global": True, "fog_density": 0.0012}, "Transform": transform((0, 0, 0))}),
            entity("MainCamera", {"Camera": {"$v": 1, "far_plane": 1000, "fov_degrees": 60, "near_plane": 0.1, "ortho_height": 10.0, "priority": 0,
                                             "projection": "Perspective"},
                                  "Transform": transform(camera_from, quat(yaw, pitch)), "Tags": {"$v": 1, "names": ["MainCamera"]}}),
        ],
    }


# Meadow (area 0) and overlook (area 4) as BuildWorld picks them for the showcase seed.
MEADOW, OVERLOOK = (52.0, 9.6, 336.0), (468.0, 49.9, 172.0)
SCENES = {
    "Valley": ((440.0, 78.0, 150.0), (200.0, 10.0, 300.0)),                # from above the overlook, across the whole valley
    "Meadow": ((MEADOW[0], MEADOW[1] + 1.7, MEADOW[2]), (150.0, 20.0, 200.0)),  # at the spawn, looking toward the woodland
}

PROJECT = {
    "$type": "Project", "$v": 1,
    "always_cook": ["Scenes/Valley.ascene", "Scenes/Meadow.ascene"],
    "collision_matrix": [], "default_quality": "High", "engine_version": "0.29.0", "fixed_timestep_hz": 60.0, "gravity": [0.0, -9.81, 0.0],
    "icon": "", "layers": ["Default"], "name": "Forest Valley", "plugins": [], "startup_scene": "Scenes/Valley.ascene", "version": "0.1.0",
    "vsync": True, "window_height": 720, "window_title": "Forest Valley", "window_width": 1280,
}

if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "Content", "Scenes"), exist_ok=True)
    for name, (frm, to) in SCENES.items():
        with open(os.path.join(ROOT, "Content", "Scenes", name + ".ascene"), "w", newline="\n") as f:
            json.dump(scene(frm, to), f, indent=2)
            f.write("\n")
    # Quality presets are the engine's standard four, copied from the one place they are written down.
    ashen = os.path.join(ROOT, "..", "..", "games", "ASHEN-FRONTIER", "ASHEN-FRONTIER.aproject")
    with open(ashen) as f:
        PROJECT["quality_presets"] = json.load(f)["quality_presets"]
    with open(os.path.join(ROOT, "ForestValley.aproject"), "w", newline="\n") as f:
        json.dump(PROJECT, f, indent=2, sort_keys=True)
        f.write("\n")
    print("wrote", ", ".join(SCENES), "and ForestValley.aproject")
