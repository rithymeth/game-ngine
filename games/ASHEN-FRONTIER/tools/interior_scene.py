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


def kael(s, position, yaw, script, components=0):
    player = s.add("Kael", {"CharacterMovement": {"$v": 1, "acceleration": 20.0, "air_acceleration": 5.0, "air_control": 0.3, "braking": 25.0, "coyote_time": 0.1, "flying": False,
                                                   "gravity_scale": 1.0, "height": 1.8, "jump_buffer": 0.1, "jump_velocity": 5.0, "mass": 70.0, "max_slope_degrees": 45.0,
                                                   "radius": 0.35, "run_speed": 7.0, "step_height": 0.35, "walk_speed": 4.0},
                              "ScriptComponent": {"$v": 1, "properties": [], "script": script}}, position=position, rotation=(0, yaw, 0), tags=("Player",))
    s.model("KaelBody", "Kael", position=(0, 0, 0), tags=("PlayerBody",), snap=False)
    s.entities[-1]["components"]["Parent"] = {"$v": 1, "parent": player}
    give_hands(s, components)
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
