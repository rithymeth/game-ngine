"""Phase 3: the on-screen UI (UI/Hud.aui) and the font it needs, written through the MCP server."""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mcpc import Mcp

REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GAME = os.path.abspath(os.path.join(HERE, ".."))
if not os.path.exists(os.path.join(GAME, "STARDUST-HOP.aproject")):  # run from the scratch copy
    REPO = r"D:\game-ngine\.claude\worktrees\sample-game-127bbf"
    GAME = os.path.join(REPO, "games", "STARDUST-HOP")

m = Mcp()
assert m.call("project_open", {"project_file": os.path.join(GAME, "STARDUST-HOP.aproject")})

# the player draws UI text with the project's fonts/Roboto-Medium.ttf (the engine sample's copy, Apache-licensed)
font_dir = os.path.join(REPO, "games", "AETHER-01", "Content", "fonts")
r = m.call("asset_add_files", {"sources": [os.path.join(font_dir, "Roboto-Medium.ttf"), os.path.join(font_dir, "LICENSE-Roboto.txt")], "folder": "fonts", "overwrite": True})
assert r, "font import failed"

# a coin icon for the counter: drawn here with PIL (an anti-aliased gold disc with a rim and a star-ish highlight)
from PIL import Image, ImageDraw
SS = 4
icon = Image.new("RGBA", (64 * SS, 64 * SS), (0, 0, 0, 0))
d = ImageDraw.Draw(icon)
d.ellipse((2 * SS, 2 * SS, 62 * SS, 62 * SS), fill=(176, 120, 20, 255))
d.ellipse((6 * SS, 6 * SS, 58 * SS, 58 * SS), fill=(255, 205, 64, 255))
d.ellipse((12 * SS, 12 * SS, 52 * SS, 52 * SS), outline=(214, 150, 32, 255), width=3 * SS)
d.polygon([(32 * SS, 16 * SS), (37 * SS, 28 * SS), (50 * SS, 28 * SS), (40 * SS, 36 * SS), (44 * SS, 49 * SS), (32 * SS, 41 * SS), (20 * SS, 49 * SS), (24 * SS, 36 * SS), (14 * SS, 28 * SS), (27 * SS, 28 * SS)], fill=(255, 238, 150, 255))
os.makedirs(os.path.join(HERE, "_art"), exist_ok=True)
icon_path = os.path.join(HERE, "_art", "coin.png")
icon.resize((64, 64), Image.LANCZOS).save(icon_path)
assert m.call("asset_add_files", {"sources": [icon_path], "folder": "UI", "overwrite": True}), "icon import failed"

GOLD = [1.0, 0.82, 0.25, 1.0]
hud = {
    "type": "Canvas", "name": "Hud",
    "children": [
        {"type": "Border", "name": "CoinPanel", "background": {"kind": "Color", "tint": [0.02, 0.04, 0.09, 0.72]}, "padding": [22, 14, 22, 16],
         "slot": {"anchors": [0, 0, 0, 0], "position": [28, 28], "size": [420, 140]},
         "children": [{"type": "VerticalBox", "children": [
             {"type": "HorizontalBox", "children": [
                 {"type": "Image", "name": "CoinIcon", "brush": {"kind": "Image", "image": "UI/coin.png", "tint": [1, 1, 1, 1]}, "desired_size": [44, 44]},
                 {"type": "Spacer", "size": [12, 12]},
                 {"type": "Text", "name": "CoinText", "text": "COINS  0 / 15", "size": 38, "color": GOLD},
             ]},
             {"type": "Spacer", "size": [10, 12]},
             {"type": "ProgressBar", "name": "CoinBar", "percent": 0.0,
              "background": {"kind": "Color", "tint": [0.13, 0.15, 0.2, 1.0]}, "bar": {"kind": "Color", "tint": GOLD},
              "slot": {"size": [376, 16]}},
         ]}]},
        {"type": "Text", "name": "Message", "text": "", "size": 64, "color": [1.0, 1.0, 1.0, 1.0], "justify": "Center",
         "slot": {"anchors": [0.5, 0.2, 0.5, 0.2], "alignment": [0.5, 0.5], "auto_size": True}},
        {"type": "Text", "name": "Hint", "text": "WASD  RUN     SPACE  JUMP     MOUSE  LOOK     R  RESTART", "size": 26, "color": [0.85, 0.9, 1.0, 0.8], "justify": "Center",
         "slot": {"anchors": [0.5, 1, 0.5, 1], "alignment": [0.5, 1], "position": [0, -26], "auto_size": True}},
    ],
}
layout = {"$type": "Layout", "version": 1, "root": hud}
checked = m.call("ui_check", {"definition": layout})
print("ui_check:", json.dumps({k: checked[k] for k in ("ok", "problems", "widgets")}))
assert checked and checked["ok"], checked
assert m.call("content_write", {"path": "UI/Hud.aui", "text": json.dumps(layout, indent=1)})
prev = m.images("ui_preview", {"definition": layout, "width": 960, "height": 540})
if prev:
    import base64
    os.makedirs(os.path.join(HERE, "_shots"), exist_ok=True)
    open(os.path.join(HERE, "_shots", "hud_preview.png"), "wb").write(base64.b64decode(prev[0]["data"]))

settings = m.call("project_info")
cook = sorted(set(settings["always_cook"]) | {"UI/", "fonts/"})
assert m.call("project_set", {"always_cook": cook})
print("always_cook:", cook)
m.close()
