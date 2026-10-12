"""A bot that plays STARDUST-HOP through the MCP server (no window): walks the platforms in order, jumping, and reports what happened."""
import json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mcpc import Mcp
import level

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
GAME = os.path.join(REPO, "games", "STARDUST-HOP")
OUT = os.path.join(HERE, "_cooked")
SHOTS = os.path.join(HERE, "_shots")
os.makedirs(SHOTS, exist_ok=True)
FRAMES = 6
DT = 1 / 60

m = Mcp()
assert m.call("project_open", {"project_file": os.path.join(GAME, "STARDUST-HOP.aproject")})
cook = m.call("asset_cook", {"output_dir": OUT}) if "--nocook" not in sys.argv else {"pak_file": os.path.join(OUT, "Game.apak")}
load = m.call("game_load", {"paks": [cook.get("pak_file", OUT)], "user_dir": os.path.join(HERE, "_user")})
assert load and not load["errors"], load

pid = m.call("game_list_entities", {"tag": "Player"})["entities"][0]["entity"]


def pos():
    e = m.call("game_get_entity", {"entity": pid})
    return e["components"]["Transform"]["position"]


def coins_left():
    ents = m.call("game_list_entities", {"tag": "Coin", "limit": 100})["entities"]
    n = 0
    for e in ents:
        g = m.call("game_get_entity", {"entity": e["entity"]})
        if g["components"]["Transform"]["position"][1] > -100:
            n += 1
    return n


def shot(name):
    m.call("game_screenshot", {"path": os.path.join(SHOTS, name + ".png"), "width": 960, "height": 540, "inline": False})


m.call("game_step", {"frames": 20, "dt": DT})
targets = [(p[0], p[1], p[2], p[3]) for p in level.PLATFORMS[1:]]
extra = {}
jumped_at = -9
t = 0.0
trace = []
for name, tx, tz, top in targets:
    stuck = 0
    best = 1e9
    for step in range(400):
        x, y, z = pos()
        dx, dz = tx - x, tz - z
        dist = math.hypot(dx, dz)
        if dist < 1.0 and abs(y - top) < 0.6:
            break
        keys = {"W": 0, "S": 0, "A": 0, "D": 0, "Space": 0}
        if dz < -0.35: keys["W"] = 1
        if dz > 0.35: keys["S"] = 1
        if dx > 0.35: keys["D"] = 1
        if dx < -0.35: keys["A"] = 1
        want_jump = dist < 4.0 and y < top + 0.1 and t - jumped_at > 0.8
        if want_jump:
            keys["Space"] = 1
            jumped_at = t
        m.call("game_input", {"set": keys})
        m.call("game_step", {"frames": FRAMES, "dt": DT})
        t += FRAMES * DT
        if dist < best - 0.05:
            best = dist; stuck = 0
        else:
            stuck += 1
        if y < -8:
            print("fell near", name, [round(v, 1) for v in (x, y, z)])
        if stuck > 120:
            print("stuck near", name, [round(v, 1) for v in (x, y, z)])
            break
    x, y, z = pos()
    trace.append((name, [round(v, 2) for v in (x, y, z)], round(t, 1)))
    print("reached" if math.hypot(tx - x, tz - z) < 1.5 else "MISSED", name, [round(v, 2) for v in (x, y, z)], "t=%.1f" % t)
    if name in ("P6", "P12"):
        shot("bot_" + name)

m.call("game_input", {"clear": True})
m.call("game_step", {"frames": 30, "dt": DT})
shot("bot_goal")
print("coins left:", coins_left(), "of 15")
m.call("game_step", {"frames": 60, "dt": DT})
shot("bot_confetti")
state = m.call("game_state")
print("state:", json.dumps({k: state[k] for k in ("script_errors", "warnings", "time") if k in state}))
m.close()
