#!/usr/bin/env python3
"""Writes the scenes of Ashen Frontier into Content/Scenes (they are generated: a layout is code, a change is a diff).

    python tools/make_scenes.py            # ModelGallery and FallenBasin
"""

import json
import math
import pathlib
import random
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from scenelib import Scene, give_hands, mule, workbench, generator, beacon  # noqa: E402
import world  # noqa: E402
import world_scene  # noqa: E402
import interior_scene  # noqa: E402

OUT = pathlib.Path(__file__).resolve().parent.parent / "Content" / "Scenes"

SAND = (0.55, 0.30, 0.18)  # oxidised red ground


def model_gallery():
    """The machines and props in two rows, close up, to look at them (and to see what changes when one is replaced)."""
    s = Scene()
    s.sky(1.2)
    s.sun(yaw=-25, pitch=-38)
    s.model("Ground", "Ground", position=(0, 0, -6), scale=(60, 1, 40), color=SAND)
    for i, name in enumerate(("Kael", "MULE3", "SPARK2", "SENTINEL4", "Drone")):
        s.model("G_" + name, name, position=(-4.4 + i * 2.2, 0, 0))
    props = ("ScrapPile", "SalvageCrate", "Pylon", "Generator", "FabBench", "RelayNode", "HazardNode", "Scrub")
    for i, name in enumerate(props):
        s.model("G_" + name, name, position=(-6.3 + i * 1.8, 0, -3.6))
    s.camera((0.2, 1.9, 6.4), yaw=0, pitch=-9, fov=58)
    s.save(OUT / "ModelGallery.ascene")


def large_gallery():
    """The big pieces: rocks, a cliff section, a ruin wall, the outpost pad and the crashed freighter."""
    s = Scene()
    s.sky(1.2)
    s.sun(yaw=-25, pitch=-38)
    s.model("Ground", "Ground", position=(0, 0, -20), scale=(160, 1, 120), color=SAND)
    s.model("G_RockA", "RockA", position=(-13, 0, 2))
    s.model("G_RockB", "RockB", position=(-7, 0, 2))
    s.model("G_Cliff", "Cliff", position=(2, 0, 1))
    s.model("G_RuinWall", "RuinWall", position=(10, 0, 3))
    s.model("G_OutpostPlate", "OutpostPlate", position=(18, 0, 1))
    s.camera((3.0, 12.0, 24.0), yaw=0, pitch=-16, fov=62)
    s.save(OUT / "ModelGalleryLarge.ascene")


def freighter_view():
    """The crashed freighter alone, from three-quarters above."""
    s = Scene()
    s.sky(1.2)
    s.sun(yaw=-25, pitch=-38)
    s.model("Ground", "Ground", position=(0, 0, 0), scale=(300, 1, 300), color=SAND)
    s.model("G_Freighter", "Freighter", position=(0, 0, 0), rotation=(0, 20, 0))
    s.camera((60, 26, 62), yaw=40, pitch=-18, fov=60)
    s.save(OUT / "ModelFreighter.ascene")


def script_guid(name):
    """The asset GUID of a script, read from its .ameta (the engine writes it when the project is first scanned)."""
    meta = json.loads((OUT.parent / "Scripts" / (name + ".luau.ameta")).read_text(encoding="utf-8"))
    return meta["guid"]


def fallen_basin(playable=True):
    """The Fallen Basin: 200 x 200 m of oxidised ground walled by cliffs, with the crashed freighter as its landmark, the outpost pad beside it,
    three salvage sites, an electrical-storm hazard zone, the MULE-3 wreck, a distress relay and the ruins of a homestead.
    Playable: Kael with the third-person controller. Otherwise a static high camera, for an overview picture."""
    rng = random.Random(9)
    s = Scene()
    s.sky(1.0)
    s.sun(yaw=-30, pitch=-34, intensity=3.4)
    s.add("Atmosphere", {"PostProcessVolume": {"$v": 1, "blend_distance": 1.0, "bloom_intensity": 0.10, "exposure_compensation": 0.0, "extents": [5.0, 5.0, 5.0], "fog_density": 0.0008,
                                                "global": True, "height_fog_base": 0.0, "height_fog_density": 0.0, "height_fog_falloff": 0.02, "priority": 0,
                                                "saturation": 1.05, "tonemapper": "ACES", "vignette": 0.12, "weight": 1.0}})
    # the floor, and its collider
    s.model("Ground", "Ground", position=(0, 0, 0), scale=(200, 1, 200), color=SAND, tags=("Ground",))
    s.add("GroundCollider", {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": 0.8, "half_extents": [100, 0.5, 100], "is_trigger": False, "report_stay": False, "restitution": 0.0}}, position=(0, -0.5, 0))

    # the walls: cliff sections all round, and one collider along each side
    for side, (cx, cz, along, yaw) in enumerate(((0, -99, "x", 0), (0, 99, "x", 180), (-99, 0, "z", 90), (99, 0, "z", 270))):
        for k in range(8):
            offset = -87.5 + k * 25
            pos = (offset, 0, cz) if along == "x" else (cx, 0, offset)
            s.model(f"Cliff_{side}_{k}", "Cliff", position=pos, rotation=(0, yaw + rng.uniform(-6, 6), 0), scale=(2.1, rng.uniform(1.5, 2.1), 2.6), color=(0.50 + rng.uniform(-0.04, 0.04), 0.29, 0.19))
        half = (100, 6, 4) if along == "x" else (4, 6, 100)
        s.add(f"CliffCollider_{side}", {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": 0.5, "half_extents": list(half), "is_trigger": False, "report_stay": False, "restitution": 0.0}},
              position=(cx, 6, cz))

    # the landmark: the freighter, broken in two, and the outpost pad where the player settles
    s.model("Freighter", "Freighter", position=(-42, 0, -34), rotation=(0, 28, 0), tags=("Landmark",), collider=((0, 5, 6), (6.5, 5, 14)))
    s.model("OutpostPad", "OutpostPlate", position=(6, 0, 14), tags=("OutpostPad",))
    for i, (dx, dz) in enumerate(((-6, 0), (6, 0))):
        s.add(f"PadLight_{i}", {"PointLight": {"$v": 1, "cast_shadows": False, "color": [0.2, 0.5, 1.0], "intensity": 6.0, "range": 12.0}}, position=(6 + dx, 2.0, 14 + dz))
    # a line of pylons from the pad toward the wreck: the grid the first generator will join
    for i in range(5):
        s.model(f"Pylon_{i}", "Pylon", position=(-2 - i * 8, 0, 8 - i * 6.5), rotation=(0, rng.uniform(-10, 10), 0), collider=((0, 2, 0), (0.4, 2, 0.4)))

    # the first machine, found in the wreckage, and the salvage sites
    mule(s, "MULE3_Wreck", (-20, 0, -12), rotation=(0, 70, 8))
    for i, (x, z) in enumerate(((-28, -10), (30, -30), (-58, 26))):
        s.model(f"SalvageSite_{i}_Pile", "ScrapPile", position=(x, 0, z), rotation=(0, rng.uniform(0, 360), 0), tags=("Salvage",), collider=((0, 0.6, 0), (1.2, 0.7, 1.0)))
        s.model(f"SalvageSite_{i}_Crate", "SalvageCrate", position=(x + 2.4, 0, z + 1.2), rotation=(0, rng.uniform(0, 90), 0), tags=("Salvage",), collider=((0, 0.4, 0), (0.45, 0.4, 0.3)))
        s.add(f"SalvageSite_{i}_Glow", {"PointLight": {"$v": 1, "cast_shadows": False, "color": [0.2, 0.5, 1.0], "intensity": 4.0, "range": 7.0}}, position=(x + 0.4, 1.4, z - 0.2))

    # the distress relay the first chain leads to, and the hazard zone on the way to it
    s.model("DistressRelay", "RelayNode", position=(62, 0, -48), tags=("Relay",), collider=((0, 2, 0), (1.0, 2.5, 1.0)))
    s.add("DistressRelay_Light", {"PointLight": {"$v": 1, "cast_shadows": False, "color": [1.0, 0.55, 0.15], "intensity": 8.0, "range": 12.0}}, position=(62, 5.0, -48))
    for i, (x, z) in enumerate(((34, 38), (42, 32), (38, 46))):
        s.model(f"HazardNode_{i}", "HazardNode", position=(x, 0, z), tags=("Hazard",), collider=((0, 1.5, 0), (0.35, 1.5, 0.35)))
        s.add(f"HazardGlow_{i}", {"PointLight": {"$v": 1, "cast_shadows": False, "color": [1.0, 0.5, 0.1], "intensity": 9.0, "range": 10.0}}, position=(x, 3.8, z))

    # a homestead in ruins, rocks, and dry scrub, scattered (kept off the pad and the spawn)
    for i, (x, z, yaw) in enumerate(((-70, -60, 10), (-64, -60, -4), (-72, -53, 92), (-65, -52, 85))):
        s.model(f"Ruin_{i}", "RuinWall", position=(x, 0, z), rotation=(0, yaw, 0), collider=((0, 1.0, 0), (2.5, 1.0, 0.25)))
    def clear(x, z):
        return all(math.hypot(x - cx, z - cz) > r for cx, cz, r in ((6, 14, 14), (-42, -34, 36), (62, -48, 10), (38, 38, 14)))
    placed = 0
    while placed < 26:
        x, z = rng.uniform(-92, 92), rng.uniform(-92, 92)
        if not clear(x, z):
            continue
        big = rng.random() < 0.3
        s.model(f"Rock_{placed}", "RockB" if big else "RockA", position=(x, 0, z), rotation=(0, rng.uniform(0, 360), 0), scale=(1, 1, 1) if big else (0.8, 0.8, 0.8),
                collider=((0, 1.2, 0), (1.6, 1.2, 1.6)) if big else ((0, 0.8, 0), (1.2, 0.8, 1.0)))
        placed += 1
    for i in range(40):
        x, z = rng.uniform(-92, 92), rng.uniform(-92, 92)
        if clear(x, z):
            s.model(f"Scrub_{i}", "Scrub", position=(x, 0, z), rotation=(0, rng.uniform(0, 360), 0), scale=(1.2, 1.2, 1.2))

    if playable:
        player = s.add("Kael", {"CharacterMovement": {"$v": 1, "acceleration": 20.0, "air_acceleration": 5.0, "air_control": 0.3, "braking": 25.0, "coyote_time": 0.1, "flying": False,
                                                       "gravity_scale": 1.0, "height": 1.8, "jump_buffer": 0.1, "jump_velocity": 5.0, "mass": 70.0, "max_slope_degrees": 45.0,
                                                       "radius": 0.35, "run_speed": 7.0, "step_height": 0.35, "walk_speed": 4.0},
                                  "ScriptComponent": {"$v": 1, "properties": [], "script": script_guid("ThirdPersonController")}},
                       position=(10, 0.05, 20), rotation=(0, 200, 0), tags=("Player",))
        s.model("KaelBody", "Kael", position=(0, 0, 0), tags=("PlayerBody",)).__class__  # placeholder to keep the call chain short
        s.entities[-1]["components"]["Parent"] = {"$v": 1, "parent": player}
        give_hands(s)
        s.camera((10, 3.2, 26), yaw=0, pitch=-12, fov=62)
        s.save(OUT / "FallenBasin.ascene")
    else:
        s.model("Kael", "Kael", position=(10, 0, 20), rotation=(0, 200, 0))
        s.camera((52, 46, 70), yaw=24, pitch=-30, fov=62, far=2500)
        s.save(OUT / "FallenBasinOverview.ascene")


def open_world():
    """OpenWorld (playable), an overview from above and the top-down map (an orthographic camera, rendered to Content/Textures/WorldMap.png), and world.json."""
    world_scene.build(True, {"script": script_guid("ThirdPersonController")}, door_script=script_guid("Door")).save(OUT / "OpenWorld.ascene")
    world_scene.build(False, {"position": (150, 230, 330), "yaw": 24, "pitch": -30, "clear": True}).save(OUT / "OpenWorldOverview.ascene")
    world_scene.build(False, {"ortho": True}).save(OUT / "WorldMapTopDown.ascene")
    interior_scene.assembly_hall(script_guid("ThirdPersonController"), script_guid("Door")).save(OUT / "AssemblyHall.ascene")
    data = OUT.parent / "Data"
    data.mkdir(exist_ok=True)
    (data / "world.json").write_text(json.dumps(world.world_data(), indent=2) + "\n", encoding="utf-8", newline="\n")


def salvage_test():
    """A small test scene: Kael beside a salvage crate on a floor (the smoke test presses Interact and expects the crate to be taken)."""
    s = Scene()
    s.sky(1.2)
    s.sun(yaw=-25, pitch=-38)
    s.model("Ground", "Ground", position=(0, 0, 0), scale=(60, 1, 60), color=SAND, snap=False)
    s.add("GroundCollider", {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": 0.8, "half_extents": [30, 0.5, 30], "is_trigger": False, "report_stay": False, "restitution": 0.0}}, position=(0, -0.5, 0))
    s.model("Test_Crate", "SalvageCrate", position=(1.5, 0, 0), tags=("Salvage",), snap=False)
    interior_scene.kael(s, (0, 0.1, 0), 0, script_guid("ThirdPersonController"))
    s.save(OUT / "SalvageTest.ascene")


def mule_test():
    """Kael with three components beside the broken MULE-3, 2.9 m away on a floor: Interact repairs it and it walks up to him."""
    s = Scene()
    s.sky(1.2)
    s.sun(yaw=-25, pitch=-38)
    s.model("Ground", "Ground", position=(0, 0, 0), scale=(60, 1, 60), color=SAND, snap=False)
    s.add("GroundCollider", {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": 0.8, "half_extents": [30, 0.5, 30], "is_trigger": False, "report_stay": False, "restitution": 0.0}}, position=(0, -0.5, 0))
    mule(s, "Test_MULE3", (2.9, 0, 0), snap=False)
    interior_scene.kael(s, (0, 0.1, 0), 0, script_guid("ThirdPersonController"), components=3)
    s.save(OUT / "MuleTest.ascene")


def craft_test():
    """Two tiny scenes for the smoke test: Kael with the cost of a Generator Core beside the fab bench; and beside the generator (run after the first,
    in the same user directory, the saved Generator Core is what powers it)."""
    for name, build in (("CraftTest", lambda s: workbench(s, "Test_FabBench", (2.5, 0, 0), snap=False)),
                        ("PowerTest", lambda s: generator(s, "Test_Generator", (2.5, 0, 0), snap=False))):
        s = Scene()
        s.sky(1.2)
        s.sun(yaw=-25, pitch=-38)
        s.model("Ground", "Ground", position=(0, 0, 0), scale=(60, 1, 60), color=SAND, snap=False)
        s.add("GroundCollider", {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": 0.8, "half_extents": [30, 0.5, 30], "is_trigger": False, "report_stay": False, "restitution": 0.0}}, position=(0, -0.5, 0))
        build(s)
        interior_scene.kael(s, (0, 0.1, 0), 0, script_guid("ThirdPersonController"), components=2, scrap=4)
        s.save(OUT / (name + ".ascene"))


def beacon_test():
    """Kael 3 m from a relay: reaching it needs no key, just standing near."""
    s = Scene()
    s.sky(1.2)
    s.sun(yaw=-25, pitch=-38)
    s.model("Ground", "Ground", position=(0, 0, 0), scale=(60, 1, 60), color=SAND, snap=False)
    s.add("GroundCollider", {"BoxCollider": {"$v": 1, "center": [0, 0, 0], "friction": 0.8, "half_extents": [30, 0.5, 30], "is_trigger": False, "report_stay": False, "restitution": 0.0}}, position=(0, -0.5, 0))
    beacon(s, "Test_Relay", "RelayNode", (3, 0, 0), snap=False)
    interior_scene.kael(s, (0, 0.1, 0), 0, script_guid("ThirdPersonController"))
    s.save(OUT / "BeaconTest.ascene")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    model_gallery()
    large_gallery()
    freighter_view()
    fallen_basin(True)
    fallen_basin(False)
    open_world()
    salvage_test()
    mule_test()
    craft_test()
    beacon_test()
    print("wrote", OUT)


if __name__ == "__main__":
    main()
