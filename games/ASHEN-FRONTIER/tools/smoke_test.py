#!/usr/bin/env python3
"""Ashen Frontier's smoke test: the layout is consistent, the project cooks, and the real player walks Kael across the open world.

    python tools/smoke_test.py            # needs the engine built (build/ at the repository root, or AETHER_BUILD=<dir>) and xvfb-run on Linux

Checks, in order (it stops at the first that fails and says which):
  1. layout   world.json agrees with tools/world.py; every region has 4 to 6 points of interest, each inside its region; each point has scene
              content within 40 m; no ridge collider closes a pass.
  2. cook     the project cooks (the player reads the cooked package).
  3. play     the player loads OpenWorld, holds W for 300 frames: Kael ends on the ground (not fallen through, not floating), has moved from the
              spawn, is inside the world, no script or load errors were logged, and the screenshot is not blank.
"""

import json
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "tools"))
import terrain  # noqa: E402
import world  # noqa: E402

BUILD = pathlib.Path(os.environ.get("AETHER_BUILD", HERE.parent.parent / "build"))
failures = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


def layout():
    data = json.loads((HERE / "Content" / "Data" / "world.json").read_text(encoding="utf-8"))
    check(data == world.world_data(), "Content/Data/world.json is what tools/world.py says (run tools/make_scenes.py to refresh it)")
    scene = json.loads((HERE / "Content" / "Scenes" / "OpenWorld.ascene").read_text(encoding="utf-8"))
    positions = [e["components"]["Transform"]["position"] for e in scene["entities"] if "ModelRenderer" in e["components"]]
    for region in data["regions"]:
        pois = [p for p in data["pois"] if p["region"] == region["id"]]
        check(4 <= len(pois) <= 6, f"{region['name']} has 4 to 6 points of interest ({len(pois)})")
        cx, cz = region["centre"]
        for p in pois:
            x, z = p["position"]
            inside = abs(x - cx) <= region["half"] and abs(z - cz) <= region["half"]
            near = any(math.hypot(x - q[0], z - q[2]) < 40 for q in positions)
            check(inside and near, f"  {p['name']} is inside {region['name']} and has content within 40 m")
    # the passes are open: the ground at the middle of each is walkable, not a cliff
    for gate in data["passes"]:
        px, pz = gate["position"]
        gentle = float(terrain.slope(px, pz)) < 0.6
        check(gentle, f"the pass between {gate['between'][0]} and {gate['between'][1]} is not a cliff (slope {float(terrain.slope(px, pz)):.2f})")
    # the ground a character walks on is the ground drawn: the collision grid and the mesh come from the same function (spot check at the spawn)
    grid = json.loads((HERE / "Content" / "Data" / "terrain_collision.json").read_text(encoding="utf-8"))
    n, cell, o = grid["samples"], grid["cell_size"], grid["origin"][0]
    sx, _y, sz = world.SPAWN["position"]
    i, j = round((sx - o) / cell), round((sz - o) / cell)
    check(abs(grid["heights"][j * n + i] - float(terrain.height(o + i * cell, o + j * cell))) < 0.01, "Content/Data/terrain_collision.json is what tools/terrain.py says")


def cook(work):
    exe = BUILD / "tools" / "cook" / "aether_cook"
    result = subprocess.run([str(exe), str(HERE / "ASHEN-FRONTIER.aproject"), "--out", str(work / "cooked")], capture_output=True, text=True)
    return check(result.returncode == 0, "the project cooks") or print(result.stdout[-2000:], result.stderr[-2000:])


def read_bmp(path):
    raw = path.read_bytes()
    offset = int.from_bytes(raw[10:14], "little")
    width, height = int.from_bytes(raw[18:22], "little"), abs(int.from_bytes(raw[22:26], "little", signed=True))
    bpp = int.from_bytes(raw[28:30], "little") // 8
    return raw, offset, width, height, bpp


def play(work):
    player = BUILD / "player" / "aether_player"
    shot = work / "shot.bmp"
    command = [str(player), "--pak", str(work / "cooked"), "--scene", "Scenes/OpenWorld.ascene", "--frames", "300", "--size", "640x360", "--press", "W",
               "--report", "--screenshot", str(shot), "--user-dir", str(work / "user")]
    if sys.platform.startswith("linux") and shutil.which("xvfb-run"):
        command = ["xvfb-run", "-a"] + command
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    text = result.stdout + result.stderr
    check(result.returncode == 0, f"the player runs and exits cleanly (exit {result.returncode})")
    problems = [l for l in text.splitlines() if re.search(r"\[(ERROR|WARN)\].*(script|Script|luau|Luau|scene|Scene|load|Load)", l) and "Vulkan" not in l and "settings" not in l]
    check(not problems, "no script, scene or load errors in the log" + ("" if not problems else ": " + problems[0][:140]))
    match = re.search(r"Player at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text)
    if not check(match is not None, "the player reports where Kael ended up"):
        return
    x, y, z = (float(v) for v in match.groups())
    sx, sy, sz = world.SPAWN["position"]
    ground = float(terrain.height(x, z))
    check(ground - 0.5 <= y <= ground + 1.5, f"Kael is on the ground, not fallen through or floating (y = {y:.2f}, the terrain there is {ground:.2f})")
    check(math.hypot(x - sx, z - sz) >= 1.5, f"Kael walked from the spawn ({math.hypot(x - sx, z - sz):.1f} m)")
    check(abs(x) < world.HALF and abs(z) < world.HALF, "Kael is inside the world")
    if shot.exists():
        raw, offset, width, height, bpp = read_bmp(shot)
        step = max(1, (width * height) // 4000)
        colours = {raw[offset + i * bpp: offset + i * bpp + 3] for i in range(0, width * height, step)}
        check(len(colours) > 60, f"the screenshot is not blank ({len(colours)} distinct colours sampled)")
    else:
        check(False, "the player wrote a screenshot")


def interior(work):
    """Kael starts in the Assembly Hall and walks out of its exit door: the Door script's Scene.Load takes the game to the open world."""
    command = [str(BUILD / "player" / "aether_player"), "--pak", str(work / "cooked"), "--scene", "Scenes/AssemblyHall.ascene", "--frames", "400", "--size", "320x180",
               "--press", "S", "--report", "--user-dir", str(work / "user2")]
    if sys.platform.startswith("linux") and shutil.which("xvfb-run"):
        command = ["xvfb-run", "-a"] + command
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    text = result.stdout + result.stderr
    check(result.returncode == 0, f"the player runs the hall and exits cleanly (exit {result.returncode})")
    loads = re.findall(r"Loaded (Scenes/\S+\.ascene)", text)
    check(loads[:2] == ["Scenes/AssemblyHall.ascene", "Scenes/OpenWorld.ascene"], f"the exit door leads from the hall to the open world (loaded: {loads})")
    check(not re.search(r"\[ERROR\]", text), "no errors on the way through the door")
    match = re.search(r"Player at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text)
    if check(match is not None, "the player reports where Kael ended up after the door"):
        x, _, z = (float(v) for v in match.groups())
        hx, hz = world.poi_position("assembly_hall")
        check(math.hypot(x - hx, z - hz) < 45, f"Kael comes out at the hall's door, not at the world's spawn ({math.hypot(x - hx, z - hz):.0f} m from the hall)")


def salvage(work):
    """Kael stands beside a salvage crate and presses Interact: the Interactor script takes it into the pack. The pack is saved (SaveGames, the
    "autosave" slot), so a second run in the same user directory starts with what the first one took."""
    def run(frames):
        command = [str(BUILD / "player" / "aether_player"), "--pak", str(work / "cooked"), "--scene", "Scenes/SalvageTest.ascene", "--frames", str(frames), "--size", "320x180",
                   "--press", "E", "--report", "--user-dir", str(work / "user3")]
        if sys.platform.startswith("linux") and shutil.which("xvfb-run"):
            command = ["xvfb-run", "-a"] + command
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        return result, result.stdout + result.stderr
    result, text = run(60)
    check(result.returncode == 0, f"the player runs the salvage test and exits cleanly (exit {result.returncode})")
    check("Salvaged Test_Crate: +2 components (pack: 0 scrap, 2 components)" in text, "pressing Interact beside a crate takes it (+2 components)")
    check(not re.search(r"\[ERROR\]", text), "no errors while salvaging")
    result, text = run(60)
    check("Salvaged Test_Crate: +2 components (pack: 0 scrap, 4 components)" in text, "the pack was saved: the next run starts with the 2 components of the first")


def mule(work):
    """Kael has three components beside the broken MULE-3 (2.9 m away) and presses Interact: it is repaired (the components are spent) and walks up to him."""
    command = [str(BUILD / "player" / "aether_player"), "--pak", str(work / "cooked"), "--scene", "Scenes/MuleTest.ascene", "--frames", "200", "--size", "320x180",
               "--press", "E", "--report", "--user-dir", str(work / "user4")]
    if sys.platform.startswith("linux") and shutil.which("xvfb-run"):
        command = ["xvfb-run", "-a"] + command
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    text = result.stdout + result.stderr
    check(result.returncode == 0, f"the player runs the MULE-3 test and exits cleanly (exit {result.returncode})")
    check("MULE-3 repaired" in text, "Interact beside the broken MULE-3 with enough components repairs it")
    check("MULE-3 reached Kael" in text, "the repaired MULE-3 follows Kael and comes up to him")
    check(not re.search(r"\[ERROR\]", text), "no errors with MULE-3")


def main():
    layout()
    if failures:
        return 1
    work = pathlib.Path(tempfile.mkdtemp(prefix="ashen-smoke-"))
    try:
        if cook(work):
            play(work)
            interior(work)
            salvage(work)
            mule(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{len(failures)} check(s) failed" if failures else "smoke test passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
