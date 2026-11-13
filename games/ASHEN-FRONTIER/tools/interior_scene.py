"""The interiors of Ashen Frontier: rooms the player walks into through a door (Scene.Load), each its own scene.

An interior is a floor, four walls (slabs with a box collider each), machines from the model set and a few coloured lights; the dark and the
cobalt/amber light are what make it read as inside (the reference: a rusted assembly hall with machines in rows and a robot standing in the aisle).
"""

import json
import math
import random

from scenelib import Scene, give_hands
import world_scene

LIGHT, BLUE, AMBER = world_scene.LIGHT, world_scene.BLUE, world_scene.AMBER
RUST = (0.32, 0.16, 0.10)
DARK = (0.16, 0.15, 0.15)


def _box(s, name, centre, half):
    s.add(name, {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": 0.6, "half_extents": list(half), "is_trigger": False, "report_stay": False, "restitution": 0.0}}, position=centre)


def room(s, width, length, height, floor=DARK, wall=RUST):
    """Floor, ceiling and four walls of a room centred on the origin (x across, z along), with colliders."""
    s.model("Floor", "Ground", position=(0, 0, 0), scale=(width, 1, length), color=floor, tags=("Ground",), snap=False)
    _box(s, "FloorCollider", (0, -0.5, 0), (width / 2 + 1, 0.5, length / 2 + 1))
    s.model("Ceiling", "Ground", position=(0, height, 0), rotation=(180, 0, 0), scale=(width, 1, length), color=DARK, snap=False)
    for name, (cx, cz, sx, sz, yaw) in {"North": (0, -length / 2, width, 0, 0), "South": (0, length / 2, width, 0, 180),
                                         "West": (-width / 2, 0, length, 0, 90), "East": (width / 2, 0, length, 0, 270)}.items():
        span = sx
        s.model("Wall" + name, "Ground", position=(cx, height / 2, cz), rotation=(90, yaw, 0), scale=(span, 1, height), color=wall, snap=False)
        half = (span / 2, height / 2, 0.5)
        if name in ("West", "East"):
            half = (0.5, height / 2, span / 2)
        _box(s, name + "Collider", (cx + (-0.5 if name == "West" else 0.5 if name == "East" else 0), height / 2, cz + (-0.5 if name == "North" else 0.5 if name == "South" else 0)), half)


def kael(s, position, yaw, script, components=0, scrap=0):
    player = s.add("Kael", {"CharacterMovement": {"$v": 1, "acceleration": 20.0, "air_acceleration": 5.0, "air_control": 0.3, "braking": 25.0, "coyote_time": 0.1, "flying": False,
                                                   "gravity_scale": 1.0, "height": 1.8, "jump_buffer": 0.1, "jump_velocity": 5.0, "mass": 70.0, "max_slope_degrees": 45.0,
                                                   "radius": 0.35, "run_speed": 7.0, "step_height": 0.35, "walk_speed": 4.0},
                              "ScriptComponent": {"$v": 1, "properties": [], "script": script}}, position=position, rotation=(0, yaw, 0), tags=("Player",))
    s.model("KaelBody", "Kael", position=(0, 0, 0), tags=("PlayerBody",), snap=False)
    s.entities[-1]["components"]["Parent"] = {"$v": 1, "parent": player}
    give_hands(s, components, scrap)
    s.camera((position[0], 3.0, position[2] + 6), yaw=0, pitch=-12, fov=62, far=300)


def door(s, name, position, target, script, radius=2.2, spawn=""):
    """The doorway: a cobalt glow, a light, and the Door script that loads `target` when the player walks in."""
    s.model(name + "Frame", "Pylon", position=(position[0] - 1.9, 0, position[2]), snap=False)
    s.model(name + "FrameB", "Pylon", position=(position[0] + 1.9, 0, position[2]), snap=False)
    s.add(name, {"ScriptComponent": {"$v": 1, "script": script, "properties": [{"name": "target", "value": json.dumps(target)}, {"name": "radius", "value": str(radius)}, {"name": "spawn", "value": json.dumps(spawn)}]}},
          position=position)
    s.add(name + "Light", LIGHT(BLUE, 10.0, 10.0), position=(position[0], 3.0, position[2] - 1.5))


def assembly_hall(player_script, door_script):
    """The buried assembly line: a 24 x 40 m hall, machines in two rows with an aisle between, a gantry overhead, MULE-3's sibling standing in the aisle,
    amber work lights and cobalt doorway. Kael comes in at the south end; the door back out is behind him."""
    rng = random.Random(5)
    s = Scene()
    s.sky(0.12, color=(0.5, 0.6, 0.8))
    s.add("Atmosphere", {"PostProcessVolume": {"$v": 1, "blend_distance": 1.0, "bloom_intensity": 0.25, "exposure_compensation": 0.6, "extents": [5.0, 5.0, 5.0], "fog_density": 0.012,
                                                "global": True, "height_fog_base": 0.0, "height_fog_density": 0.0, "height_fog_falloff": 0.02, "priority": 0,
                                                "saturation": 1.05, "tonemapper": "ACES", "vignette": 0.25, "weight": 1.0}})
    W, L, H = 24.0, 40.0, 9.0
    room(s, W, L, H)
    # two rows of machines along the walls, their work lights between them
    for side, x in enumerate((-8.0, 8.0)):
        for i in range(5):
            z = -15 + i * 7
            kind = ("FabBench", "Generator")[(i + side) % 2]
            s.model(f"Machine_{side}_{i}", kind, position=(x, 0, z), rotation=(0, 90 if side == 0 else 270, 0), snap=False,
                    collider=((0, 1.0, 0), (1.4, 1.0, 1.4)))
            s.add(f"WorkLight_{side}_{i}", LIGHT(AMBER, 9.0, 10.0), position=(x * 0.55, 6.5, z + 3.5))
    # columns down the aisle edges, a gantry across the middle, scrap on the floor
    for i in range(4):
        for x in (-4.0, 4.0):
            s.model(f"Column_{x}_{i}", "Pylon", position=(x, 0, -12 + i * 9), snap=False, scale=(1.4, 2.0, 1.4), collider=((0, 4, 0), (0.6, 4, 0.6)))
    s.model("Gantry", "Gantry", position=(0, 0, -2), rotation=(0, 90, 0), snap=False, scale=(1.0, 1.0, 1.0))
    for i in range(7):
        s.model(f"Scrap_{i}", "ScrapPile", position=(rng.uniform(-10, 10), 0, rng.uniform(-18, 14)), rotation=(0, rng.uniform(0, 360), 0), snap=False)
    # the robot in the aisle, and the glow of the far end
    s.model("Guardian", "SENTINEL4", position=(0, 0, -12), rotation=(0, 0, 0), snap=False, tags=("Enemy",), collider=((0, 1.2, 0), (0.8, 1.2, 0.8)))
    s.add("GuardianLight", LIGHT(BLUE, 14.0, 14.0), position=(0, 3.5, -16))
    s.add("EntryLight", LIGHT(BLUE, 8.0, 12.0), position=(0, 4.0, 15))
    # the way out, behind the player
    door(s, "ExitDoor", (0, 0, 18.5), "Scenes/OpenWorld.ascene", door_script, spawn="AssemblyHallOutside")
    s.add("HallEntry", {}, position=(0, 0.1, 13.0))
    kael(s, (0, 0.1, 13.0), 180, player_script)
    return s


GREEN = (0.3, 1.0, 0.6)


def living_dome(player_script, door_script):
    """The living dome's inside: a 30 x 40 m garden hall under a dark roof, a tall glowing green core in the middle, a ring of trees and scrub round it,
    maintenance drones hanging in the air, green light from the core. Kael comes in at the south; the door out is behind him."""
    rng = random.Random(11)
    s = Scene()
    s.sky(0.15, color=(0.5, 0.8, 0.6))
    s.add("Atmosphere", {"PostProcessVolume": {"$v": 1, "blend_distance": 1.0, "bloom_intensity": 0.35, "exposure_compensation": 0.5, "extents": [5.0, 5.0, 5.0], "fog_density": 0.010,
                                                "global": True, "height_fog_base": 0.0, "height_fog_density": 0.0, "height_fog_falloff": 0.02, "priority": 0,
                                                "saturation": 1.15, "tonemapper": "ACES", "vignette": 0.25, "weight": 1.0}})
    W, L, H = 30.0, 40.0, 14.0
    room(s, W, L, H, floor=(0.20, 0.26, 0.15), wall=(0.16, 0.22, 0.20))
    # the core: a tall cylinder of green light, with a ring of low planters round its foot
    s.model("Core", "Pylon", position=(0, 0, -3), snap=False, scale=(3.2, 3.2, 3.2), color=(0.1, 0.3, 0.2), emissive=(0.2, 1.0, 0.5), collider=((0, 6, 0), (1.5, 6, 1.5)))
    s.add("CoreLight", LIGHT(GREEN, 40.0, 28.0), position=(0, 7.0, -3))
    s.add("CoreLightLow", LIGHT(GREEN, 18.0, 18.0), position=(0, 1.8, -3))
    import math
    for i in range(14):
        angle = i / 14 * math.tau
        r = 7.0 + (i % 2) * 1.6
        x, z = math.cos(angle) * r, -3 + math.sin(angle) * r
        s.model(f"Tree_{i}", "TreeLow", position=(x, 0, z), rotation=(0, rng.uniform(0, 360), 0), scale=(1.3, rng.uniform(1.0, 1.7), 1.3), snap=False, collider=((0, 1.5, 0), (0.4, 1.5, 0.4)))
    for i in range(24):
        x, z = rng.uniform(-13, 13), rng.uniform(-13, 8)
        if math.hypot(x, z + 3) > 4.5:
            s.model(f"Scrub_{i}", "Scrub", position=(x, 0, z), rotation=(0, rng.uniform(0, 360), 0), scale=(1.4, 1.4, 1.4), snap=False)
    for i in range(4):
        s.model(f"Drone_{i}", "Drone", position=(math.cos(i * 1.6) * 5.5, 4.2 + (i % 2) * 1.0, -3 + math.sin(i * 1.6) * 5.5), rotation=(0, i * 70, 0), snap=False)
    s.add("EntryLight", LIGHT((0.4, 0.8, 1.0), 8.0, 12.0), position=(0, 4.0, 13))
    door(s, "ExitDoor", (0, 0, 18.5), "Scenes/OpenWorld.ascene", door_script, spawn="LivingDomeOutside")
    s.add("DomeEntry", {}, position=(0, 0.1, 12.0))
    kael(s, (0, 0.1, 12.0), 180, player_script)
    return s


def reactor_hall(player_script, door_script):
    """The reactor hall under the Hollow Core's gate: a vast 50 x 60 m, 30 m high chamber with the reactor column standing in the middle in a pool of cobalt
    light, pillars round the walls, gantries crossing overhead, two sentinels on guard. Kael comes in at the south; the door out is behind him."""
    import math
    rng = random.Random(17)
    s = Scene()
    s.sky(0.08, color=(0.4, 0.55, 0.9))
    s.add("Atmosphere", {"PostProcessVolume": {"$v": 1, "blend_distance": 1.0, "bloom_intensity": 0.45, "exposure_compensation": 0.7, "extents": [5.0, 5.0, 5.0], "fog_density": 0.014,
                                                "global": True, "height_fog_base": 0.0, "height_fog_density": 0.0, "height_fog_falloff": 0.02, "priority": 0,
                                                "saturation": 1.1, "tonemapper": "ACES", "vignette": 0.3, "weight": 1.0}})
    W, L, H = 50.0, 60.0, 30.0
    room(s, W, L, H, floor=(0.12, 0.13, 0.17), wall=(0.13, 0.14, 0.2))
    s.model("Reactor", "ReactorTower", position=(0, 0, -8), snap=False, scale=(1.3, 1.1, 1.3), collider=((0, 15, 0), (6, 15, 6)))
    s.add("ReactorLightHigh", LIGHT((0.15, 0.5, 1.0), 80.0, 60.0), position=(0, 18, -8))
    s.add("ReactorLightLow", LIGHT((0.15, 0.5, 1.0), 40.0, 40.0), position=(0, 3, -8))
    for i in range(10):
        angle = i / 10 * math.tau
        x, z = math.cos(angle) * 21, -8 + math.sin(angle) * 24
        s.model(f"Pillar_{i}", "Pylon", position=(x, 0, z), snap=False, scale=(3.0, 6.0, 3.0), collider=((0, 6, 0), (1.3, 6, 1.3)))
    for i, z in enumerate((-22, 6)):
        s.model(f"Gantry_{i}", "Gantry", position=(0, 9 + i * 4, z), rotation=(0, 90, 0), snap=False, scale=(1.6, 1.0, 1.6))
    for i, (x, z) in enumerate(((-9, 6), (9, 6))):
        s.model(f"Guard_{i}", "SENTINEL4", position=(x, 0, z), rotation=(0, 180, 0), snap=False, tags=("Enemy",), collider=((0, 1.2, 0), (0.8, 1.2, 0.8)))
    for i in range(6):
        s.model(f"Debris_{i}", "ScrapPile", position=(rng.uniform(-18, 18), 0, rng.uniform(8, 22)), rotation=(0, rng.uniform(0, 360), 0), snap=False)
    s.add("EntryLight", LIGHT(BLUE, 10.0, 14.0), position=(0, 5.0, 25))
    door(s, "ExitDoor", (0, 0, 27.5), "Scenes/OpenWorld.ascene", door_script, spawn="ReactorHallOutside")
    s.add("ReactorEntry", {}, position=(0, 0.1, 22.0))
    kael(s, (0, 0.1, 22.0), 180, player_script)
    return s
