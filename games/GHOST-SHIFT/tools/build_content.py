#!/usr/bin/env python3
"""Ghost Shift content generator: the Willow House graybox and the kit assets.

Writes everything under games/GHOST-SHIFT/Content that is not hand-written (the Luau scripts in
Content/Scripts are hand-written and only get their .ameta here). Run it from anywhere:

    python3 games/GHOST-SHIFT/tools/build_content.py

Every GUID is derived with uuid5 from a stable key, so rerunning produces identical files.
Existing .ameta files are left alone (the cooker adds model sub-asset GUIDs to them on the first cook).
"""
import base64
import json
import math
import os
import struct
import uuid
import wave

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CONTENT = os.path.join(ROOT, "Content")
NS = uuid.UUID("6f1d3c3e-9b0a-4a55-8c1e-67686f737473")  # "ghosts"


def guid(key):
    return str(uuid.uuid5(NS, "ghost-shift:" + key))


def write(rel, text):
    path = os.path.join(CONTENT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def write_json(rel, data):
    write(rel, json.dumps(data, indent=2) + "\n")


def meta(rel, importer, version=1):
    """The asset's .ameta with a deterministic GUID; returns the GUID. Never overwrites."""
    g = guid("asset:" + rel)
    path = os.path.join(CONTENT, rel + ".ameta")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)["guid"]
    write_json(rel + ".ameta", {"$type": "AssetMeta", "$v": 1, "guid": g, "importer": importer,
                                "importer_version": version, "labels": [], "settings": {}, "source_hash": ""})
    return g


# ---------------------------------------------------------------- models (flat-colour unit boxes)

def box_mesh():
    faces = [((0, 0, 1), [(-.5, -.5, .5), (.5, -.5, .5), (.5, .5, .5), (-.5, .5, .5)]),
             ((0, 0, -1), [(.5, -.5, -.5), (-.5, -.5, -.5), (-.5, .5, -.5), (.5, .5, -.5)]),
             ((1, 0, 0), [(.5, -.5, .5), (.5, -.5, -.5), (.5, .5, -.5), (.5, .5, .5)]),
             ((-1, 0, 0), [(-.5, -.5, -.5), (-.5, -.5, .5), (-.5, .5, .5), (-.5, .5, -.5)]),
             ((0, 1, 0), [(-.5, .5, .5), (.5, .5, .5), (.5, .5, -.5), (-.5, .5, -.5)]),
             ((0, -1, 0), [(-.5, -.5, -.5), (.5, -.5, -.5), (.5, -.5, .5), (-.5, -.5, .5)])]
    pos, nor, uv, idx = [], [], [], []
    for n, quad in faces:
        base = len(pos)
        for p, t in zip(quad, [(0, 0), (1, 0), (1, 1), (0, 1)]):
            pos.append(p), nor.append(n), uv.append(t)
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nor, uv, idx


def gltf_box(name, rgba, emissive=None):
    pos, nor, uv, idx = box_mesh()
    buf = bytearray()
    views = []

    def view(data):
        while len(buf) % 4:
            buf.append(0)
        views.append({"buffer": 0, "byteOffset": len(buf), "byteLength": len(data)})
        buf.extend(data)
        return len(views) - 1

    pv = view(b"".join(struct.pack("<3f", *p) for p in pos))
    nv = view(b"".join(struct.pack("<3f", *n) for n in nor))
    tv = view(b"".join(struct.pack("<2f", *t) for t in uv))
    iv = view(b"".join(struct.pack("<I", i) for i in idx))
    mat = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(rgba), "metallicFactor": 0.0, "roughnessFactor": 0.9}}
    if emissive:
        mat["emissiveFactor"] = list(emissive)
    doc = {
        "asset": {"version": "2.0", "generator": "ghost shift build_content.py"},
        "buffers": [{"uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode("ascii"), "byteLength": len(buf)}],
        "bufferViews": views,
        "accessors": [
            {"bufferView": pv, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": [-.5, -.5, -.5], "max": [.5, .5, .5]},
            {"bufferView": nv, "componentType": 5126, "count": len(nor), "type": "VEC3"},
            {"bufferView": tv, "componentType": 5126, "count": len(uv), "type": "VEC2"},
            {"bufferView": iv, "componentType": 5125, "count": len(idx), "type": "SCALAR"},
        ],
        "materials": [mat],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0, "mode": 4}]}],
        "nodes": [{"name": name, "mesh": 0}],
        "scenes": [{"nodes": [0]}], "scene": 0,
    }
    return json.dumps(doc, separators=(",", ":")) + "\n"


MODELS = {
    "floor": ((0.36, 0.30, 0.25, 1), None),
    "wall": ((0.58, 0.55, 0.62, 1), None),
    "basement": ((0.30, 0.31, 0.33, 1), None),
    "lawn": ((0.18, 0.26, 0.17, 1), None),
    "stairs": ((0.45, 0.33, 0.22, 1), None),
    "furniture": ((0.42, 0.24, 0.20, 1), None),
    "van": ((0.85, 0.85, 0.80, 1), None),
    "ectoplasm": ((0.35, 1.0, 0.45, 1), (0.10, 0.60, 0.15)),
    "stain": ((0.45, 0.10, 0.55, 1), (0.15, 0.02, 0.20)),
    "hatch": ((0.75, 0.62, 0.20, 1), None),
    "ghost": ((0.80, 0.95, 1.0, 1), (0.30, 0.45, 0.55)),
    "tool": ((0.95, 0.75, 0.10, 1), (0.40, 0.30, 0.00)),
    "marker": ((0.20, 0.35, 0.80, 1), None),
}
MODEL_GUID = {}


def build_models():
    for name, (rgba, emissive) in MODELS.items():
        rel = f"models/gs_{name}.gltf"
        write(rel, gltf_box("gs_" + name, rgba, emissive))
        MODEL_GUID[name] = meta(rel, "Model", 4)


# ---------------------------------------------------------------- audio (synthesized, deterministic)

def synth(rel, seconds, fn, rate=22050):
    path = os.path.join(CONTENT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    frames = bytearray()
    n = int(seconds * rate)
    for i in range(n):
        t = i / rate
        env = min(1.0, t / 0.05, (seconds - t) / 0.1)
        v = max(-1.0, min(1.0, fn(t) * env))
        frames += struct.pack("<h", int(v * 32000))
    with wave.open(path, "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(rate)
        w.writeframes(bytes(frames))
    meta(rel, "Sound")


def cue(name, spatial, volume_db, looping=False, max_distance=25.0):
    rel = f"Audio/{name}.acue"
    write_json(rel, {
        "name": name,
        "nodes": [{"id": 1, "pos": [0.0, 0.0], "sound": f"Audio/{name}.wav", "type": "Wave", "looping": looping}],
        "output": {"attenuation": {"lowpass_at_max_hz": 0.0, "max_distance": max_distance, "min_distance": 1.0, "model": "Inverse", "rolloff": 1.0},
                   "bus": "SFX", "doppler": 1.0, "occlusion": False, "pitch": 1.0, "priority": 128, "spatial": spatial,
                   "spatial_blend": 1.0 if spatial else 0.0, "virtual": "Continue", "volume_db": volume_db},
        "root": 1, "version": 1})
    meta(rel, "SoundCue")


def build_audio():
    # The Prankster's warning moan: a falling, wobbling tone (spatial, so players can find it).
    synth("Audio/moan.wav", 1.5, lambda t: 0.6 * math.sin(2 * math.pi * (220 - 60 * t) * t + 3 * math.sin(2 * math.pi * 5 * t)))
    synth("Audio/vacuum.wav", 0.4, lambda t: 0.35 * math.sin(2 * math.pi * 140 * t) + 0.15 * math.sin(2 * math.pi * 283 * t))
    synth("Audio/stun.wav", 0.35, lambda t: 0.5 * math.sin(2 * math.pi * (900 - 1800 * t) * t))
    synth("Audio/ambient.wav", 4.0, lambda t: 0.08 * math.sin(2 * math.pi * 55 * t) * (0.6 + 0.4 * math.sin(2 * math.pi * 0.25 * t)))
    cue("moan", True, -2.0)
    cue("vacuum", False, -8.0)
    cue("stun", False, -6.0)
    cue("ambient", False, -14.0, looping=True)


# ---------------------------------------------------------------- input

def action(name, value_type):
    rel = f"Input/{name}.aaction"
    write_json(rel, {"$type": "InputAction", "data": {"$v": 1, "name": name, "value_type": value_type}})
    meta(rel, "InputAction")


def modifier(kind):
    return {"$v": 1, "lower": 0.2, "negate_x": True, "negate_y": True, "negate_z": True, "order": "YXZ",
            "scale": [1.0, 1.0, 1.0], "type": kind, "upper": 1.0}


def binding(act, key, *mods):
    return {"$v": 1, "action": act, "actuation": 0.5, "key": key, "modifiers": [modifier(m) for m in mods], "triggers": []}


def build_input():
    action("Move", "Axis2D")
    action("Look", "Axis2D")
    action("Interact", "Bool")
    action("UseTool", "Bool")
    action("SwitchTool", "Bool")
    bindings = [
        binding("Move", "W", "Swizzle"), binding("Move", "S", "Swizzle", "Negate"), binding("Move", "D"), binding("Move", "A", "Negate"),
        binding("Move", "GamepadLeftStickX", "DeadZone"), binding("Move", "GamepadLeftStickY", "DeadZone", "Swizzle", "Negate"),
        binding("Look", "MouseX"), binding("Look", "MouseY", "Swizzle"),
        binding("Look", "GamepadRightStickX", "DeadZone"), binding("Look", "GamepadRightStickY", "DeadZone", "Swizzle"),
        binding("Interact", "E"), binding("Interact", "GamepadX"),
        binding("UseTool", "MouseLeft"), binding("UseTool", "GamepadRightTrigger"),
        binding("SwitchTool", "Q"), binding("SwitchTool", "GamepadY"),
    ]
    rel = "Input/Gameplay.amapping"
    write_json(rel, {"$type": "InputMappingContext", "data": {"$v": 1, "bindings": bindings, "block_lower_contexts": False,
                                                              "consume_input": True, "name": "Gameplay"}})
    meta(rel, "InputMapping")


# ---------------------------------------------------------------- gameplay kit assets

def build_gameplay():
    def put(rel, importer, data):
        write_json(rel, data)
        meta(rel, importer)

    # The vacuum pulse: battery pays, the tank fills. Luau only activates it on a patch with residue left.
    put("Gameplay/VacuumPulseCost.aeffect", "GameplayEffect", {
        "name": "VacuumPulseCost", "duration_policy": "instant",
        "modifiers": [{"attribute": "Battery", "op": "add", "magnitude": -2}, {"attribute": "Tank", "op": "add", "magnitude": 1}]})
    put("Gameplay/VacuumPulse.aability", "GameplayAbility", {"name": "VacuumPulse", "cost": "VacuumPulseCost"})
    put("Gameplay/StunBlastCost.aeffect", "GameplayEffect", {
        "name": "StunBlastCost", "duration_policy": "instant", "modifiers": [{"attribute": "Battery", "op": "add", "magnitude": -4}]})
    put("Gameplay/StunBlast.aability", "GameplayAbility", {"name": "StunBlast", "cost": "StunBlastCost"})
    put("Gameplay/ContractPayout.aeffect", "GameplayEffect", {
        "name": "ContractPayout", "duration_policy": "instant", "modifiers": [{"attribute": "Currency", "op": "add", "magnitude": 250}]})
    put("Gameplay/WillowCleanup.aquest", "QuestDefinition", {
        "name": "WillowCleanup", "title": "Willow House: Basic Cleanup",
        "description": "Vacuum the ectoplasm, keep the Prankster in check and get back to the van.",
        "objectives": [
            {"id": "clean_ectoplasm", "kind": "count", "target": "ectoplasm", "required": 4, "text": "Vacuum the ectoplasm patches"},
            {"id": "scrub_stain", "kind": "flag", "text": "Scrub the cursed stain with the UV scrubber", "optional": True},
            {"id": "extract", "kind": "flag", "text": "Return to the van and extract"},
        ],
        # Unknown keys are ignored by the quest loader: the payout must be under rewards.effects.
        "rewards": {"effects": ["ContractPayout"]}})
    put("Gameplay/SpectralVacuum.aitem", "ItemDefinition", {"name": "SpectralVacuum", "max_stack": 1, "weight": 6.0, "equip_slot": "MainHand"})
    put("Gameplay/UVScrubber.aitem", "ItemDefinition", {"name": "UVScrubber", "max_stack": 1, "weight": 2.0, "equip_slot": "MainHand"})


# ---------------------------------------------------------------- the scene

def quat_y(deg):
    h = math.radians(deg) / 2
    return [0.0, math.sin(h), 0.0, math.cos(h)]


ENTITIES = []


def entity(key, components, tags=()):
    comps = {"Transform": components.pop("Transform")} if "Transform" in components else {}
    comps.update(components)
    if tags:
        comps["Tags"] = {"$v": 1, "names": list(tags)}
    ENTITIES.append({"guid": guid("entity:" + key), "components": comps})


def transform(pos, rot=None, scale=None):
    t = {"$v": 1, "position": [float(v) for v in pos], "rotation": rot or [0.0, 0.0, 0.0, 1.0]}
    if scale is not None:
        t["scale"] = [float(v) for v in scale]
    return t


def model(name):
    return {"$v": 1, "model": MODEL_GUID[name], "asset_path": f"models/gs_{name}.gltf"}


def solid(key, lo, hi, look, walkable=False, collide=True):
    """An axis-aligned box from lo to hi: a scaled visual entity plus an unscaled collider entity.

    The collider stays unscaled because the player's nav bake and AI line of sight read
    BoxCollider half_extents at the Transform position and ignore Transform scale/rotation."""
    center = [(a + b) / 2 for a, b in zip(lo, hi)]
    size = [b - a for a, b in zip(lo, hi)]
    entity(key + ".mesh", {"Transform": transform(center, scale=size), "ModelRenderer": model(look)})
    if collide:
        entity(key + ".col", {"Transform": transform(center),
                              "BoxCollider": {"$v": 1, "center": [0.0, 0.0, 0.0], "friction": 0.6, "half_extents": [s / 2 for s in size],
                                              "is_trigger": False, "report_stay": False, "restitution": 0.0}},
               ["NavWalkable"] if walkable else [])


def wall_x(key, x, z0, z1, y0, y1, gaps=(), t=0.2, look="wall"):
    """A wall along z at x, with door gaps (z ranges)."""
    edges = [z0] + [v for g in gaps for v in g] + [z1]
    for i in range(0, len(edges), 2):
        if edges[i + 1] - edges[i] > 0.01:
            solid(f"{key}.{i // 2}", (x - t / 2, y0, edges[i]), (x + t / 2, y1, edges[i + 1]), look)


def wall_z(key, z, x0, x1, y0, y1, gaps=(), t=0.2, look="wall"):
    edges = [x0] + [v for g in gaps for v in g] + [x1]
    for i in range(0, len(edges), 2):
        if edges[i + 1] - edges[i] > 0.01:
            solid(f"{key}.{i // 2}", (edges[i], y0, z - t / 2), (edges[i + 1], y1, z + t / 2), look)


PATCHES = [(-3.0, 0.0, -3.0), (3.0, 0.0, -5.0), (-3.0, 0.0, -12.0), (0.0, -3.0, -13.0)]
STAIN = (-4.5, 0.0, -6.5)
HIDING_SPOTS = [("Couch", (-5.0, 0.0, -1.5)), ("KitchenCupboard", (-5.0, 0.0, -15.0)),
                ("BasementShelf", (-5.0, -3.0, -9.5)), ("AtticHatch", (4.0, 3.0, -6.0))]
WAYPOINTS = [(0.0, 1.2, -4.0), (-3.0, 1.2, -10.0), (2.0, 1.2, -14.0), (0.0, -1.8, -12.0), (2.5, 1.2, -2.0)]
VAN = (0.0, 0.0, 9.0)
PLAYER_START = (0.0, 0.05, 4.0)


def build_scene(script_guids):
    ENTITIES.clear()
    entity("moon", {"Transform": transform((0, 0, 0), [-0.38, 0.2, 0.08, 0.9]),
                    "DirectionalLight": {"$v": 1, "color": [0.55, 0.62, 0.9], "intensity": 0.9, "cast_shadows": False}}, ["Moon"])
    entity("sky", {"Transform": transform((0, 0, 0)), "SkyLight": {"$v": 1, "color": [0.25, 0.28, 0.45], "intensity": 0.5}}, ["Sky"])
    entity("camera", {"Transform": transform((0, 1.65, 4)),
                      "Camera": {"$v": 1, "projection": "Perspective", "fov_degrees": 75.0, "near_plane": 0.05, "far_plane": 300.0,
                                 "ortho_height": 10.0, "priority": 0},
                      "AudioListener": {"$v": 1, "active": True}}, ["MainCamera"])
    entity("ambient", {"Transform": transform((0, 0, 0)), "AudioSource": {"$v": 1, "cue": "Audio/ambient.acue", "auto_play": True, "volume_db": 0, "pitch": 1}}, ["AmbientAudio"])
    entity("sfx.vacuum", {"Transform": transform((0, 0, 0)), "AudioSource": {"$v": 1, "cue": "Audio/vacuum.acue", "auto_play": False, "volume_db": 0, "pitch": 1}}, ["SfxVacuum"])
    entity("sfx.stun", {"Transform": transform((0, 0, 0)), "AudioSource": {"$v": 1, "cue": "Audio/stun.acue", "auto_play": False, "volume_db": 0, "pitch": 1}}, ["SfxStun"])

    # The player: a first-person body on the engine's Jolt character controller.
    entity("player", {"Transform": transform(PLAYER_START),
                      "CharacterMovement": {"$v": 1, "acceleration": 24.0, "air_acceleration": 6.0, "air_control": 0.4, "braking": 30.0,
                                            "coyote_time": 0.1, "flying": False, "gravity_scale": 1.0, "height": 1.75, "jump_buffer": 0.1,
                                            "jump_velocity": 4.0, "mass": 75.0, "max_slope_degrees": 50.0, "radius": 0.3,
                                            "run_speed": 3.6, "step_height": 0.35, "walk_speed": 3.6},
                      "ScriptComponent": {"$v": 1, "script": script_guids["GhostShift"], "properties": []}}, ["Player"])

    # Outside: the lawn and the company van (the tank is emptied and the contract extracted here).
    solid("lawn", (-20, -0.5, 0), (20, 0, 20), "lawn", walkable=True)
    solid("van.body", (VAN[0] - 1.1, 0, VAN[2] - 2.4), (VAN[0] + 1.1, 2.2, VAN[2] + 2.4), "van")
    entity("van", {"Transform": transform((VAN[0], 0, VAN[2] - 2.6))}, ["Van"])

    # Willow House, ground floor: living room (z 0..-8) and kitchen (z -8..-16), the stair hole at x 3..6, z -11..-16.
    solid("floor.living", (-6, -0.3, -8), (6, 0, 0), "floor", walkable=True)
    solid("floor.kitchen", (-6, -0.3, -16), (3, 0, -8), "floor", walkable=True)
    solid("floor.kitchen_east", (3, -0.3, -11), (6, 0, -8), "floor", walkable=True)
    wall_z("wall.front", 0, -6, 6, 0, 3, gaps=[(-1, 1)])
    wall_z("wall.back", -16, -6, 6, -3, 3)
    wall_x("wall.west", -6, -16, 0, -3, 3)
    wall_x("wall.east", 6, -16, 0, -3, 3)
    wall_z("wall.middle", -8, -6, 6, 0, 3, gaps=[(-1, 1)])
    solid("wall.lintel_front", (-1, 2.2, -0.1), (1, 3, 0.1), "wall")
    solid("wall.lintel_middle", (-1, 2.2, -8.1), (1, 3, -7.9), "wall")
    # Attic floor (the ceiling) and the attic hatch marker.
    solid("attic.floor.a", (-6, 3, -16), (3.4, 3.2, 0), "floor")
    solid("attic.floor.b", (4.6, 3, -16), (6, 3.2, 0), "floor")
    solid("attic.floor.c", (3.4, 3, -16), (4.6, 3.2, -6.6), "floor")
    solid("attic.floor.d", (3.4, 3, -5.4), (4.6, 3.2, 0), "floor")
    solid("attic.hatch", (3.4, 3.15, -6.6), (4.6, 3.25, -5.4), "hatch", collide=False)
    entity("attic.hatch.marker", {"Transform": transform((4.0, 3.0, -6.0))}, ["AtticHatch"])
    # Basement under the kitchen, reached by stairs down along -z on the east side.
    solid("floor.basement", (-6, -3.3, -16), (6, -3, -8), "basement", walkable=True)
    wall_z("wall.basement_north", -8, -6, 6, -3, 0, look="basement")
    for i in range(10):
        top = -0.3 * (i + 1)
        z1 = -11 - 0.5 * i
        solid(f"stairs.{i}", (3.2, -3, z1 - 0.5), (5.8, top, z1), "stairs", walkable=True)
    # Furniture (hiding spots are next to these).
    solid("couch", (-5.8, 0, -2.5), (-4.4, 0.8, -0.6), "furniture")
    solid("table", (-1.0, 0, -12.5), (1.0, 0.8, -11.5), "furniture")
    solid("cupboard", (-5.8, 0, -15.8), (-5.0, 2.0, -14.2), "furniture")
    solid("shelf", (-5.8, -3, -10.5), (-5.2, -1.2, -8.5), "furniture")

    for i, (x, y, z) in enumerate(PATCHES, 1):
        entity(f"ectoplasm.{i}", {"Transform": transform((x, y + 0.02, z), scale=(1.4, 0.04, 1.4)), "ModelRenderer": model("ectoplasm")},
               ["Ectoplasm", f"Patch{i}"])
    entity("stain", {"Transform": transform((STAIN[0], STAIN[1] + 0.02, STAIN[2]), scale=(1.0, 0.03, 1.0)), "ModelRenderer": model("stain")}, ["CursedStain"])
    for i, (name, pos) in enumerate(HIDING_SPOTS, 1):
        entity(f"hiding.{name}", {"Transform": transform(pos)}, ["HidingSpot", f"Hiding{i}", name])
    for i, pos in enumerate(WAYPOINTS, 1):
        entity(f"waypoint.{i}", {"Transform": transform(pos)}, ["PranksterWaypoint", f"Waypoint{i}"])
    # The tool the Prankster hides: parked far away until a prank.
    entity("stolen_tool", {"Transform": transform((0, -100, 0), scale=(0.5, 0.3, 0.9)), "ModelRenderer": model("tool")}, ["StolenTool"])
    # The Prankster: its light flares and its moan plays (spatially) during the warning before a prank.
    entity("prankster", {"Transform": transform(WAYPOINTS[0], scale=(0.6, 0.9, 0.6)), "ModelRenderer": model("ghost"),
                         "PointLight": {"$v": 1, "color": [0.4, 1.0, 0.7], "intensity": 0.0, "range": 8.0, "cast_shadows": False},
                         "AudioSource": {"$v": 1, "cue": "Audio/moan.acue", "auto_play": False, "volume_db": 0, "pitch": 1},
                         "ScriptComponent": {"$v": 1, "script": script_guids["Prankster"], "properties": []}}, ["Prankster", "Ghost"])

    rel = "Scenes/WillowHouse.ascene"
    write_json(rel, {"$type": "Scene", "$version": 1, "entities": ENTITIES})
    meta(rel, "Scene")


def main():
    build_models()
    build_audio()
    build_input()
    build_gameplay()
    scripts = {}
    for name in ("GhostShift", "Prankster"):
        rel = f"Scripts/{name}.luau"
        if not os.path.exists(os.path.join(CONTENT, rel)):
            raise SystemExit(f"missing hand-written script {rel}")
        scripts[name] = meta(rel, "Script")
    build_scene(scripts)
    print(f"Ghost Shift content written: {len(ENTITIES)} entities in Scenes/WillowHouse.ascene")


if __name__ == "__main__":
    main()
