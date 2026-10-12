"""Phase 1: project skeleton, models, sounds, cues, input — all written through the MCP server."""
import json, math, os, shutil, struct, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mcpc import Mcp
import geom, level

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
GAME = os.path.join(REPO, "games", "STARDUST-HOP")
TMP = os.path.join(HERE, "_sfx")
os.makedirs(GAME, exist_ok=True)
os.makedirs(os.path.join(GAME, "Content"), exist_ok=True)
os.makedirs(TMP, exist_ok=True)

# --- the project file (settings copied from the engine's sample game, then renamed) ---
src = json.load(open(os.path.join(REPO, "games", "AETHER-01", "AETHER-01.aproject"), encoding="utf-8"))
src.update({"name": "STARDUST-HOP", "window_title": "Stardust Hop", "startup_scene": "Scenes/Main.ascene",
            "always_cook": ["Input/", "Audio/"], "window_width": 1280, "window_height": 720})
project_file = os.path.join(GAME, "STARDUST-HOP.aproject")
json.dump(src, open(project_file, "w", encoding="utf-8"), indent=2)

m = Mcp()
assert m.call("project_open", {"project_file": project_file}), "project_open failed"


def put_text(path, text):
    r = m.call("content_write", {"path": path, "text": text})
    assert r, path
    return r


# --- models ---------------------------------------------------------------------------
GRASS, DIRT, GOLD, BLUE, SKIN, DARK = (0.27, 0.66, 0.30), (0.45, 0.31, 0.20), (1.0, 0.78, 0.15), (0.20, 0.42, 0.85), (0.96, 0.78, 0.62), (0.07, 0.09, 0.14)
sizes = sorted({(p[4], p[5]) for p in level.PLATFORMS})
for sx, sz in sizes:
    parts = [(geom.box(sx, 0.8, sz), DIRT, (0, -0.1, 0), None), (geom.box(sx + 0.02, 0.2, sz + 0.02), GRASS, (0, 0.4, 0), None)]
    put_text(level.platform_model(sx, sz), geom.gltf(parts, f"plat_{sx}x{sz}"))

put_text("models/coin.gltf", geom.gltf([(geom.cylinder(0.38, 0.12, 20, "z"), GOLD, (0, 0, 0), (0.55, 0.38, 0.0))], "coin"))
put_text("models/player.gltf", geom.gltf([
    (geom.cylinder(0.32, 1.25, 16), BLUE, (0, 0.65, 0), None),
    (geom.sphere(0.30), SKIN, (0, 1.58, 0), None),
    (geom.box(0.34, 0.12, 0.10), DARK, (0, 1.62, -0.27), None),     # visor: the face looks down -Z
    (geom.box(0.5, 0.12, 0.14), (0.9, 0.25, 0.25), (0, 1.0, 0), None),  # a red belt so turning is readable
], "player"))
put_text("models/goal.gltf", geom.gltf([
    (geom.cylinder(2.2, 0.2, 28), GOLD, (0, 0.1, 0), (0.25, 0.18, 0.0)),
    (geom.cylinder(0.18, 5.0, 12), (1.0, 0.95, 0.6), (0, 2.7, -1.7), (0.9, 0.85, 0.4)),
    (geom.sphere(0.45, 8, 12), (1.0, 0.95, 0.6), (0, 5.4, -1.7), (1.0, 0.9, 0.5)),
], "goal"))
put_text("models/spark.gltf", geom.gltf([(geom.box(0.18, 0.18, 0.18), (1.0, 0.85, 0.3), (0, 0, 0), (1.0, 0.8, 0.2))], "spark"))
put_text("models/checkpoint.gltf", geom.gltf([
    (geom.cylinder(0.08, 2.2, 8), (0.8, 0.8, 0.85), (0, 1.1, 0), None),
    (geom.box(0.9, 0.5, 0.05), (0.2, 0.8, 1.0), (0.45, 1.95, 0), (0.1, 0.4, 0.5)),
], "checkpoint"))
put_text("models/pillar.gltf", geom.gltf([(geom.cylinder(0.5, 6.0, 8), (0.55, 0.55, 0.62), (0, -3.0, 0), None)], "pillar"))

# --- sounds ---------------------------------------------------------------------------
RATE = 22050


def write_wav(name, samples):
    path = os.path.join(TMP, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32000)) for s in samples))
    return path


def tone(freq_a, freq_b, seconds, vol=0.5, decay=4.0):
    n = int(RATE * seconds); out = []; phase = 0.0
    for i in range(n):
        t = i / n
        f = freq_a + (freq_b - freq_a) * t
        phase += 2 * math.pi * f / RATE
        out.append(math.sin(phase) * vol * math.exp(-decay * t) * min(1.0, i / 120))
    return out


files = [
    write_wav("coin.wav", tone(880, 880, 0.08, 0.5, 1.0) + tone(1320, 1320, 0.22, 0.5, 5.0)),
    write_wav("jump.wav", tone(320, 760, 0.18, 0.35, 3.0)),
    write_wav("fall.wav", tone(520, 110, 0.55, 0.45, 2.5)),
    write_wav("checkpoint.wav", tone(660, 660, 0.12, 0.45, 2.0) + tone(990, 990, 0.3, 0.45, 5.0)),
    write_wav("win.wav", sum((tone(f, f, 0.18, 0.5, 2.5) for f in (523, 659, 784, 1047, 784, 1047)), []) + tone(1047, 1047, 0.6, 0.5, 4.0)),
]
# a slow pad for the background: a chord with a gentle tremolo, 8 seconds so it loops cleanly
pad = []
for i in range(RATE * 8):
    t = i / RATE
    env = 0.5 - 0.5 * math.cos(2 * math.pi * t / 8.0) * 0.0 + 0.0
    s = sum(math.sin(2 * math.pi * f * t) for f in (220.0, 277.18, 329.63)) / 3
    s *= 0.20 * (0.75 + 0.25 * math.sin(2 * math.pi * t / 4.0))
    fade = min(1.0, t / 0.05, (8.0 - t) / 0.05)
    pad.append(s * fade)
files.append(write_wav("ambient.wav", pad))
r = m.call("asset_add_files", {"sources": files, "folder": "Audio", "overwrite": True})
assert r, "audio import failed"


def cue(name, nodes, root, volume=0.0, bus="SFX"):
    d = m.call("kit_schema", {"type": "sound_cue"})["blank"]
    d["name"] = name; d["nodes"] = nodes; d["root"] = root
    d["output"]["bus"] = bus; d["output"]["volume_db"] = volume
    assert m.call("kit_put", {"type": "sound_cue", "path": f"Audio/{name}", "definition": d, "overwrite": True}), name


for n, vol in [("coin", -4.0), ("jump", -8.0), ("fall", -6.0), ("checkpoint", -4.0), ("win", -2.0)]:
    cue(n, [{"id": 1, "type": "Wave", "sound": f"Audio/{n}.wav", "pos": [0.0, 0.0]}], 1, vol)
cue("ambient", [{"id": 1, "type": "Loop", "count": 0, "children": [2], "pos": [0.0, 0.0]},
                {"id": 2, "type": "Wave", "sound": "Audio/ambient.wav", "pos": [0.0, 0.0]}], 1, -14.0, "Music")

# --- input (the bindings are the engine sample's, trimmed to what this game uses) -------
aether = json.load(open(os.path.join(REPO, "games", "AETHER-01", "Content", "Input", "Gameplay.amapping"), encoding="utf-8"))
keep = [b for b in aether["data"]["bindings"] if b["action"] in ("Move", "Look", "Jump")]
restart = json.loads(json.dumps(next(b for b in aether["data"]["bindings"] if b["action"] == "Restart")))
keep.append(restart)
mapping = {"$type": "InputMappingContext", "data": {"$v": 1, "bindings": keep, "block_lower_contexts": False, "consume_input": True, "name": "Gameplay"}}
for name, kind in (("Move", "Axis2D"), ("Look", "Axis2D"), ("Jump", "Bool"), ("Restart", "Bool")):
    assert m.call("kit_put", {"type": "input_action", "path": f"Input/{name}", "overwrite": True,
                              "definition": {"$type": "InputAction", "data": {"$v": 1, "name": name, "value_type": kind}}}), name
assert m.call("kit_put", {"type": "input_mapping", "path": "Input/Gameplay", "definition": mapping, "overwrite": True})

print(json.dumps(m.call("asset_list", {}) or {}, indent=0)[:1500])
m.close()
print("phase 1 done")
