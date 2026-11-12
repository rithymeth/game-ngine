"""Builds the OpenWorld scene from tools/world.py on the terrain of tools/terrain.py: the four regions, the passes between them and the central outpost.

Everything stands on terrain.height(x, z): a model is placed at its (x, z) and the ground's height is added, so moving the terrain moves the world with it.
"""

import json
import math
import random

import numpy as np

import terrain
import world
from scenelib import Scene, give_hands

LIGHT = lambda color, intensity, rng_: {"PointLight": {"$v": 1, "cast_shadows": False, "color": list(color), "intensity": intensity, "range": rng_}}
BLUE, AMBER, GREEN = (0.2, 0.5, 1.0), (1.0, 0.5, 0.1), (0.3, 1.0, 0.6)


def _collider(s, name, centre, half, friction=0.6):
    s.add(name, {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": friction, "half_extents": list(half), "is_trigger": False, "report_stay": False, "restitution": 0.0}}, position=centre, snap=True)


def _open_ground(x, z, limit=0.45):
    """True where a prop can stand: not on a cliff, and inside the walkable outline."""
    return bool(terrain.slope(x, z) < limit) and math.hypot(x, z) < 190


def _scatter(s, rng, region, prefix, models, count, scale=(0.8, 1.2), collide=True, limit=0.45):
    cx, cz = world.REGIONS[region]["centre"]
    avoid = [(x, z, r + 6) for x, z, r in terrain._FLATS]
    placed, tries = 0, 0
    while placed < count and tries < count * 60:
        tries += 1
        x, z = cx + rng.uniform(-95, 95), cz + rng.uniform(-95, 95)
        if world.region_at(x, z) != region or not _open_ground(x, z, limit) or any(math.hypot(x - ax, z - az) < r for ax, az, r in avoid):
            continue
        model = rng.choice(models)
        k = rng.uniform(*scale)
        big = model == "RockB"
        solid = collide and model.startswith("Rock")
        s.model(f"{prefix}_{placed}", model, position=(x, 0, z), rotation=(0, rng.uniform(0, 360), 0), scale=(k, k, k),
                collider=(((0, 1.2 * k, 0), (1.6 * k, 1.2 * k, 1.6 * k)) if big else ((0, 0.8 * k, 0), (1.2 * k, 0.8 * k, 1.0 * k))) if solid else None)
        placed += 1


def _outcrops(s, rng):
    """Layered rock formations (mesas, spires, arches) on the slopes and along the ridges and the rim: the canyon-wall look."""
    placed, tries = 0, 0
    tint = {"basin": None, "iron": (0.42, 0.34, 0.30), "verdant": (0.40, 0.44, 0.26), "core": (0.22, 0.22, 0.28)}
    while placed < 64 and tries < 6000:
        tries += 1
        x, z = rng.uniform(-225, 225), rng.uniform(-225, 225)
        r = math.hypot(x, z)
        sl = float(terrain.slope(x, z))
        if r > 232 or r < 36 or not (0.42 < sl < 1.5) or any(math.hypot(x - fx, z - fz) < rad + 10 for fx, fz, rad in terrain._FLATS):
            continue
        if any(math.hypot(x - o["x"], z - o["z"]) < 22 for o in s.outcrop_log):
            continue
        kind = rng.choices(("Mesa", "Spire", "Arch"), weights=(5, 4, 1))[0]
        k = rng.uniform(0.9, 1.9) if kind == "Mesa" else rng.uniform(0.9, 1.5)
        region = world.region_at(x, z) or "basin"
        s.model(f"Outcrop_{placed}", kind, position=(x, -1.2 * k, z), rotation=(0, rng.uniform(0, 360), 0), scale=(k, k * rng.uniform(0.9, 1.3), k), color=tint.get(region),
                collider=((0, 5 * k, 0), (5.0 * k, 5 * k, 5.0 * k)) if kind == "Mesa" else None)
        s.outcrop_log.append({"x": x, "z": z})
        placed += 1


def basin(s, rng):
    x, z = world.poi_position("crash_site")
    s.model("Freighter", "Freighter", position=(x, -0.6, z), rotation=(0, 28, 0), tags=("Landmark",), collider=((0, 5, 6), (6.5, 5, 14)))
    x, z = world.poi_position("mule3_wreck")
    s.model("MULE3_Wreck", "MULE3", position=(x, 0, z), rotation=(0, 70, 8), tags=("Salvage", "MULE3"), collider=((0, 0.6, 0), (0.45, 0.5, 0.7)))
    x, z = world.poi_position("homestead_ruins")
    for i, (dx, dz, yaw) in enumerate(((0, 0, 10), (6, 0, -4), (-2, 7, 92), (5, 8, 85))):
        s.model(f"Ruin_{i}", "RuinWall", position=(x + dx, 0, z + dz), rotation=(0, yaw, 0), collider=((0, 1.0, 0), (2.5, 1.0, 0.25)))
    x, z = world.poi_position("salvage_field")
    for i, (dx, dz) in enumerate(((0, 0), (14, -8), (-12, 9))):
        s.model(f"Salvage_{i}_Pile", "ScrapPile", position=(x + dx, 0, z + dz), rotation=(0, rng.uniform(0, 360), 0), tags=("Salvage",), collider=((0, 0.6, 0), (1.2, 0.7, 1.0)))
        s.model(f"Salvage_{i}_Crate", "SalvageCrate", position=(x + dx + 2.4, 0, z + dz + 1.2), rotation=(0, rng.uniform(0, 90), 0), tags=("Salvage",), collider=((0, 0.4, 0), (0.45, 0.4, 0.3)))
        s.add(f"Salvage_{i}_Glow", LIGHT(BLUE, 4.0, 7.0), position=(x + dx + 0.4, 1.4, z + dz - 0.2), snap=True)
    x, z = world.poi_position("distress_relay")
    s.model("DistressRelay", "RelayNode", position=(x, 0, z), tags=("Relay",), collider=((0, 2, 0), (1.0, 2.5, 1.0)))
    s.add("DistressRelay_Light", LIGHT((1.0, 0.55, 0.15), 8.0, 12.0), position=(x, 5.0, z), snap=True)
    x, z = world.poi_position("storm_field")
    for i, (dx, dz) in enumerate(((0, 0), (9, -6), (4, 8))):
        s.model(f"StormNode_{i}", "HazardNode", position=(x + dx, 0, z + dz), tags=("Hazard",), collider=((0, 1.5, 0), (0.35, 1.5, 0.35)))
        s.add(f"StormGlow_{i}", LIGHT(AMBER, 9.0, 10.0), position=(x + dx, 3.8, z + dz), snap=True)
    _scatter(s, rng, "fallen_basin", "BasinRock", ["RockA", "RockB", "RockA"], 22)
    _scatter(s, rng, "fallen_basin", "BasinScrub", ["Scrub"], 34, scale=(1.0, 1.4), collide=False)


def iron(s, rng, door_script=None):
    x, z = world.poi_position("smelter_row")
    for i in range(3):
        s.model(f"Smelter_{i}", "SmelterStack", position=(x + i * 15, 0, z + (i % 2) * 6), rotation=(0, i * 30, 0), tags=("Landmark",), collider=((0, 9, 0), (4, 9, 4)))
        s.add(f"SmelterGlow_{i}", LIGHT(AMBER, 18.0, 20.0), position=(x + i * 15, 19.5, z + (i % 2) * 6), snap=True)
    x, z = world.poi_position("rail_yard")
    for track in range(3):
        for car in range(4):
            s.model(f"Rail_{track}_{car}", "RailCar", position=(x + track * 6, 0, z + car * 11 - 17), tags=("Salvage",), collider=((0, 1.5, 0), (1.5, 1.5, 4.5)))
    x, z = world.poi_position("gantry_works")
    for i in range(3):
        s.model(f"Gantry_{i}", "Gantry", position=(x, 0, z + i * 22 - 22), rotation=(0, 90 if i % 2 else 0, 0), collider=((-6.5, 4.5, 0), (0.5, 4.5, 0.5)))
    x, z = world.poi_position("assembly_hall")
    s.model("AssemblyHall", "AssemblyHall", position=(x, 0, z), rotation=(0, 90, 0), tags=("Dungeon",), collider=((0, 3.5, 0), (8, 3.5, 14)))
    s.add("AssemblyDoorGlow", LIGHT(BLUE, 14.0, 14.0), position=(x + 15, 2.5, z), snap=True)
    s.add("AssemblyHallOutside", {}, position=(x + 19.5, 0.1, z), snap=True)
    if door_script:
      s.add("AssemblyDoor", {"ScriptComponent": {"$v": 1, "script": door_script, "properties": [{"name": "target", "value": json.dumps("Scenes/AssemblyHall.ascene")}, {"name": "radius", "value": "2.5"}, {"name": "spawn", "value": json.dumps("HallEntry")}]}},
            position=(x + 15.2, 0, z), snap=True)
    x, z = world.poi_position("warden_checkpoint")
    for i, (dx, dz) in enumerate(((-6, 0), (6, 0))):
        s.model(f"WardenSentinel_{i}", "SENTINEL4", position=(x + dx, 0, z + dz), rotation=(0, 180, 0), tags=("Enemy", "Warden"), collider=((0, 1.2, 0), (0.8, 1.2, 0.8)))
    for i in range(4):
        s.model(f"WardenPylon_{i}", "Pylon", position=(x - 9 + i * 6, 0, z - 8), collider=((0, 2, 0), (0.4, 2, 0.4)))
    x, z = world.poi_position("slag_pits")
    for i, (dx, dz) in enumerate(((0, 0), (14, 5), (-7, 12), (9, -12))):
        s.model(f"SlagPit_{i}", "Ground", position=(x + dx, 0.08, z + dz), scale=(9, 1, 9), color=(0.50, 0.14, 0.04), emissive=(0.35, 0.08, 0.0), tags=("Hazard",))
        s.model(f"SlagNode_{i}", "HazardNode", position=(x + dx + 5.5, 0, z + dz), tags=("Hazard",), collider=((0, 1.5, 0), (0.35, 1.5, 0.35)))
        s.add(f"SlagGlow_{i}", LIGHT((1.0, 0.4, 0.05), 14.0, 14.0), position=(x + dx, 2.0, z + dz), snap=True)
    _scatter(s, rng, "iron_wastes", "IronRock", ["RockA", "RockB"], 14)
    _scatter(s, rng, "iron_wastes", "IronScrap", ["ScrapPile"], 8, scale=(1.2, 1.6), collide=False)


def verdant(s, rng):
    x, z = world.poi_position("greenhouse_rows")
    for i in range(4):
        s.model(f"Greenhouse_{i}", "Greenhouse", position=(x + i * 16, 0, z), tags=("Landmark",), collider=((0, 2.5, 0), (6, 2.5, 10)))
        s.add(f"GreenhouseGlow_{i}", LIGHT((0.4, 1.0, 0.5), 8.0, 12.0), position=(x + i * 16, 3.0, z), snap=True)
    x, z = world.poi_position("living_dome")
    s.model("LivingDome", "Dome", position=(x, 0, z), tags=("Dungeon",), collider=((0, 3.5, 0), (8.5, 3.5, 8.5)))
    s.add("LivingDomeGlow", LIGHT(GREEN, 16.0, 20.0), position=(x, 5.0, z), snap=True)
    x, z = world.poi_position("maintenance_depot")
    s.model("DepotGenerator_0", "Generator", position=(x, 0, z), collider=((0, 0.7, 0), (0.85, 0.7, 0.65)))
    s.model("DepotGenerator_1", "Generator", position=(x + 3.5, 0, z + 1), rotation=(0, 20, 0), collider=((0, 0.7, 0), (0.85, 0.7, 0.65)))
    s.model("DepotBench", "FabBench", position=(x - 4, 0, z + 2), rotation=(0, 90, 0), collider=((0, 0.5, 0), (0.9, 0.5, 0.45)))
    for i in range(3):
        s.model(f"DepotDrone_{i}", "Drone", position=(x + i * 5 - 5, 0.8 + i * 0.3, z - 6), rotation=(0, rng.uniform(0, 360), 0), tags=("Machine",))
    x, z = world.poi_position("repaired_garden")
    for i in range(18):
        s.model(f"GardenTree_{i}", "TreeLow", position=(x + rng.uniform(-15, 15), 0, z + rng.uniform(-12, 12)), rotation=(0, rng.uniform(0, 360), 0), scale=(1.2, rng.uniform(1.0, 1.5), 1.2), collider=((0, 1.5, 0), (0.4, 1.5, 0.4)))
    x, z = world.poi_position("research_annex")
    s.model("AnnexRelay", "RelayNode", position=(x, 0, z), tags=("Relay",), collider=((0, 2, 0), (1.0, 2.5, 1.0)))
    s.model("AnnexBench", "FabBench", position=(x + 5, 0, z + 3), collider=((0, 0.5, 0), (0.9, 0.5, 0.45)))
    for i in range(3):
        s.model(f"AnnexCrate_{i}", "SalvageCrate", position=(x - 4 + i * 1.3, 0, z + 4), rotation=(0, rng.uniform(0, 90), 0), tags=("Salvage",), collider=((0, 0.4, 0), (0.45, 0.4, 0.3)))
    _scatter(s, rng, "verdant_array", "ArrayTree", ["TreeLow"], 34, scale=(0.9, 1.4))
    _scatter(s, rng, "verdant_array", "ArrayRock", ["RockA"], 8)


def hollow(s, rng):
    x, z = world.poi_position("reactor_tower")
    s.model("ReactorTower", "ReactorTower", position=(x, 0, z), tags=("Landmark",), collider=((0, 15, 0), (7, 15, 7)))
    s.add("ReactorGlow", LIGHT((0.15, 0.5, 1.0), 40.0, 45.0), position=(x, 14, z), snap=True)
    x, z = world.poi_position("data_spine")
    for i in range(8):
        a = math.radians(-70 + i * 20)
        s.model(f"DataPylon_{i}", "Pylon", position=(x + 18 * math.sin(a), 0, z + 18 * math.cos(a) - 12), rotation=(0, math.degrees(a), 0), scale=(1.4, 1.4, 1.4), collider=((0, 2, 0), (0.5, 2, 0.5)))
    x, z = world.poi_position("core_gate")
    s.model("CoreGate", "AssemblyHall", position=(x, 0, z), rotation=(0, 0, 0), scale=(1.4, 1.4, 1.0), tags=("Dungeon",), collider=((0, 3.5, 0), (11, 3.5, 14)))
    s.add("CoreGateGlow", LIGHT(BLUE, 20.0, 18.0), position=(x, 2.5, z + 16), snap=True)
    x, z = world.poi_position("failing_grid")
    for i, (dx, dz) in enumerate(((0, 0), (12, -6), (-8, 10), (6, 14))):
        s.model(f"GridNode_{i}", "HazardNode", position=(x + dx, 0, z + dz), tags=("Hazard",), collider=((0, 1.5, 0), (0.35, 1.5, 0.35)))
        s.add(f"GridGlow_{i}", LIGHT(AMBER, 14.0, 12.0), position=(x + dx, 3.8, z + dz), snap=True)
    x, z = world.poi_position("core_approach")
    for i in range(6):   # lit posts up the ramp
        s.model(f"RampPost_{i}", "Pylon", position=(x - 7 + (i % 2) * 14, 0, z - 14 - (i // 2) * 12), collider=((0, 2, 0), (0.4, 2, 0.4)))
    _scatter(s, rng, "hollow_core", "CoreRock", ["RockB", "RockA"], 14)


def build(playable=True, camera=None, door_script=None):
    rng = random.Random(21)
    s = Scene()
    s.ground = terrain.height
    s.outcrop_log = []
    s.sky(1.5, color=(1.0, 0.86, 0.70))
    s.sun(yaw=-38, pitch=-40, intensity=4.2, color=(1.0, 0.80, 0.58))
    ortho = bool(camera and camera.get("ortho"))
    s.add("Atmosphere", {"PostProcessVolume": {"$v": 1, "blend_distance": 1.0, "bloom_intensity": 0.10, "exposure_compensation": 0.0, "extents": [5.0, 5.0, 5.0], "fog_density": 0.0 if (ortho or (camera and camera.get("clear"))) else 0.0020,
                                                "global": True, "height_fog_base": 0.0, "height_fog_density": 0.0, "height_fog_falloff": 0.02, "priority": 0,
                                                "saturation": 1.08, "tonemapper": "ACES", "vignette": 0.10, "weight": 1.0}})
    # the ground: the terrain mesh with its painted texture, and the height grid the character walks on
    s.model("Terrain", "Terrain", position=(0, 0, 0), texture="Textures/TerrainColour.png", tags=("Ground",), snap=False)
    grid = terrain.collision_grid()
    s.add("TerrainCollider", {"HeightfieldCollider": {"$v": 1, "samples": grid["samples"], "cell_size": grid["cell_size"], "origin": grid["origin"], "heights": grid["heights"],
                                                       "is_trigger": False, "friction": 0.8, "restitution": 0.0, "report_stay": False}})
    # the central outpost: its pad, a ring of pylons and lights; the crossroads of the world
    s.model("OutpostPad", "OutpostPlate", position=(0, 0.02, 0), scale=(1.6, 1, 1.6), tags=("OutpostPad",))
    for i, (dx, dz) in enumerate(((-7, -7), (7, -7), (-7, 7), (7, 7))):
        s.model(f"OutpostPylon_{i}", "Pylon", position=(dx, 0, dz), collider=((0, 2, 0), (0.4, 2, 0.4)))
        s.add(f"OutpostLight_{i}", LIGHT(BLUE, 8.0, 14.0), position=(dx, 4.0, dz), snap=True)
    # a line of pylons from the Basin to the outpost: the grid the first generator joins
    bx, bz = world.poi_position("mule3_wreck")
    for i in range(8):
        t = i / 7
        s.model(f"GridPylon_{i}", "Pylon", position=(bx + (-9 - bx) * t, 0, bz + (9 - bz) * t), collider=((0, 2, 0), (0.4, 2, 0.4)))
    basin(s, rng)
    iron(s, rng, door_script)
    verdant(s, rng)
    hollow(s, rng)
    _outcrops(s, rng)
    if playable:
        player = s.add("Kael", {"CharacterMovement": {"$v": 1, "acceleration": 20.0, "air_acceleration": 5.0, "air_control": 0.3, "braking": 25.0, "coyote_time": 0.1, "flying": False,
                                                       "gravity_scale": 1.0, "height": 1.8, "jump_buffer": 0.1, "jump_velocity": 5.0, "mass": 70.0, "max_slope_degrees": 45.0,
                                                       "radius": 0.35, "run_speed": 7.0, "step_height": 0.35, "walk_speed": 4.0},
                                  "ScriptComponent": {"$v": 1, "properties": [], "script": camera["script"]}},
                       position=(world.SPAWN["position"][0], 0.15, world.SPAWN["position"][2]), rotation=(0, world.SPAWN["yaw"], 0), tags=("Player",), snap=True)
        s.model("KaelBody", "Kael", position=(0, 0, 0), tags=("PlayerBody",), snap=False)
        s.entities[-1]["components"]["Parent"] = {"$v": 1, "parent": player}
        give_hands(s)
        sx, sz = world.SPAWN["position"][0], world.SPAWN["position"][2]
        s.camera((sx, float(terrain.height(sx, sz + 6)) + 3.2, sz + 6), yaw=0, pitch=-12, fov=62, far=2500)
    else:
        sx, sz = world.SPAWN["position"][0], world.SPAWN["position"][2]
        s.model("Kael", "Kael", position=(sx, 0, sz), rotation=(0, world.SPAWN["yaw"], 0))
        if ortho:
            s.camera((0, 700, 0), yaw=0, pitch=-90, fov=60, far=3000, ortho_height=2 * (world.HALF + 20))
        else:
            s.camera(camera["position"], yaw=camera["yaw"], pitch=camera["pitch"], fov=60, far=3500)
    return s
