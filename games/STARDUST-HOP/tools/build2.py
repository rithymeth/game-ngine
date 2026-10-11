"""Phase 2: the script, the level scene, and the project settings — through the MCP server."""
import json, math, os, sys, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mcpc import Mcp
import geom, level

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
GAME = os.path.join(REPO, "games", "STARDUST-HOP")

m = Mcp()
assert m.call("project_open", {"project_file": os.path.join(GAME, "STARDUST-HOP.aproject")})

# one more model: the dark gem shown for a coin you don't have yet
assert m.call("content_write", {"path": "models/pip_on.gltf", "text": geom.gltf([(geom.box(0.12, 0.12, 0.12), (1.0, 0.8, 0.2), (0, 0, 0), (1.0, 0.75, 0.15))], "pip_on")})
assert m.call("content_write", {"path": "models/pip_off.gltf", "text": geom.gltf([(geom.box(0.12, 0.12, 0.12), (0.22, 0.24, 0.30), (0, 0, 0), None)], "pip_off")})
assert m.call("content_write", {"path": "models/sky.gltf", "text": geom.gltf([(geom.inside_sphere(320), (0.0, 0.0, 0.0), (0, 0, 0), (0.30, 0.52, 0.95))], "sky")})
assert m.call("content_write", {"path": "Scripts/Hopper.luau", "text": open(os.path.join(HERE, "Hopper.luau"), encoding="utf-8").read(), "overwrite": True}) is not None

m.call("asset_scan")


def guid_of(path):
    info = m.call("asset_info", {"asset": path})
    assert info, path
    return info["guid"]


model_guid = {}
for p in os.listdir(os.path.join(GAME, "Content", "models")):
    if p.endswith(".gltf"):
        model_guid["models/" + p] = guid_of("models/" + p)
script_guid = guid_of("Scripts/Hopper.luau")


def model(path):
    return {"$v": 1, "model": model_guid[path], "asset_path": path}


def tf(position, rotation=(0, 0, 0, 1)):
    return {"$v": 1, "position": list(position), "rotation": list(rotation)}


def euler(pitch_deg, yaw_deg):
    px, py = math.radians(pitch_deg) / 2, math.radians(yaw_deg) / 2
    qx = (math.sin(px), 0, 0, math.cos(px))
    qy = (0, math.sin(py), 0, math.cos(py))
    x1, y1, z1, w1 = qy
    x2, y2, z2, w2 = qx
    return (w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2)


entities = []


def add(components):
    entities.append({"guid": str(uuid.uuid4()), "components": components})


FAR = (0, -500, 0)
add({"Transform": tf((0, 3, 7), (-0.14943814, 0, 0, 0.9887711)),
     "Camera": {"$v": 1, "projection": "Perspective", "fov_degrees": 62.0, "near_plane": 0.1, "far_plane": 600.0, "ortho_height": 10.0, "priority": 0},
     "AudioListener": {"$v": 1, "active": True}, "Tags": {"$v": 1, "names": ["MainCamera"]}})
add({"Transform": tf((0, 0.05, 0)), "ModelRenderer": model("models/player.gltf"),
     "CharacterMovement": {"$v": 1, "acceleration": 22.0, "air_acceleration": 8.0, "air_control": 0.6, "braking": 28.0, "coyote_time": 0.12, "flying": False,
                           "gravity_scale": 1.35, "height": 1.8, "jump_buffer": 0.12, "jump_velocity": 6.2, "mass": 70.0, "max_slope_degrees": 45.0, "radius": 0.33,
                           "run_speed": 6.5, "step_height": 0.3, "walk_speed": 6.5},
     "ScriptComponent": {"$v": 1, "script": script_guid, "properties": []}, "Tags": {"$v": 1, "names": ["Player"]}})

add({"Transform": tf((0, 0, -25)), "ModelRenderer": model("models/sky.gltf"), "Tags": {"$v": 1, "names": ["Sky"]}})
add({"Transform": tf((0, 0, 0)), "AudioSource": {"$v": 1, "cue": "Audio/ambient.acue", "auto_play": True, "volume_db": 0, "pitch": 1}, "Tags": {"$v": 1, "names": ["AmbientAudio"]}})
for tag, cue in (("SfxCoin", "coin"), ("SfxJump", "jump"), ("SfxFall", "fall"), ("SfxCheckpoint", "checkpoint"), ("SfxWin", "win")):
    add({"Transform": tf((0, 0, 0)), "AudioSource": {"$v": 1, "cue": f"Audio/{cue}.acue", "auto_play": False, "volume_db": 0, "pitch": 1}, "Tags": {"$v": 1, "names": [tag]}})

for name, x, z, top, sx, sz in level.PLATFORMS:
    add({"Transform": tf((x, top - 0.5, z)), "ModelRenderer": model(level.platform_model(sx, sz)),
         "BoxCollider": {"$v": 1, "center": [0, 0, 0], "half_extents": [sx / 2, 0.5, sz / 2], "friction": 0.6, "is_trigger": False, "report_stay": False, "restitution": 0.0},
         "Tags": {"$v": 1, "names": ["Platform", name]}})

coins = level.coin_positions()
for pos in coins:
    add({"Transform": tf(pos), "ModelRenderer": model("models/coin.gltf"), "Tags": {"$v": 1, "names": ["Coin"]}})

by_name = {p[0]: p for p in level.PLATFORMS}
for name, dx, dz in (("P3", 1.2, -0.9), ("P7", 1.6, -1.6)):
    _, x, z, top, sx, sz = by_name[name]
    add({"Transform": tf((x + dx, top, z + dz)), "ModelRenderer": model("models/checkpoint.gltf"), "Tags": {"$v": 1, "names": ["Checkpoint"]}})

_, gx, gz, gtop, _, _ = by_name[level.GOAL]
add({"Transform": tf((gx, gtop, gz)), "ModelRenderer": model("models/goal.gltf"), "Tags": {"$v": 1, "names": ["Goal"]}})

for _ in coins:
    add({"Transform": tf(FAR), "ModelRenderer": model("models/pip_on.gltf"), "Tags": {"$v": 1, "names": ["PipOn"]}})
for _ in coins:
    add({"Transform": tf(FAR), "ModelRenderer": model("models/pip_off.gltf"), "Tags": {"$v": 1, "names": ["PipOff"]}})
for _ in range(40):
    add({"Transform": tf(FAR), "ModelRenderer": model("models/spark.gltf"), "Tags": {"$v": 1, "names": ["Confetti"]}})

scene = {"$type": "Scene", "$version": 1, "entities": entities}
assert m.call("content_write", {"path": "Scenes/Main.ascene", "text": json.dumps(scene, indent=1)})
print("entities:", len(entities), "coins:", len(coins))

# --- check what we made, with the tools -------------------------------------------------
chk = m.call("script_check", {"asset": "Scripts/Hopper.luau"})
print("script_check:", json.dumps(chk)[:900])
print("script_check_project:", json.dumps(m.call("script_check_project"))[:900])
print("cue:", json.dumps(m.call("cue_validate", {"asset": "Audio/coin.acue"}), indent=0)[:300] if True else "")
m.close()
