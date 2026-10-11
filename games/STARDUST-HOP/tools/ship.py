import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mcpc import Mcp
GAME = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
m = Mcp()
assert m.call("project_open", {"project_file": os.path.join(GAME, "STARDUST-HOP.aproject")})
r = m.call("asset_cook", {"output_dir": os.path.join(GAME, "dist"), "configuration": "shipping"})
print(json.dumps({k: v for k, v in r.items() if k != "assets"}, indent=1)[:1200])
m.close()
