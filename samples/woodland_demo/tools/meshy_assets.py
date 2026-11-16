#!/usr/bin/env python3
"""Generates the Woodland demo's 3D assets with Meshy (https://docs.meshy.ai), a text-to-3D service, instead of the placeholder boxes.

    python meshy_assets.py --out /tmp/woodland/meshy                 # every asset below
    python meshy_assets.py --out /tmp/woodland/meshy --only crate    # one

Each asset is a text prompt. Meshy makes an untextured *preview* mesh, then a *refine* pass textures it (PBR); the glb is downloaded to OUT/<name>.glb.
This costs Meshy credits (check `--balance` first). A rerun costs nothing for what is done: the task ids are kept in OUT/state.json, so an interrupted run
picks up where it stopped, and an asset already downloaded is not asked for again (delete its entry to remake it).

The script sends no API key. In this environment a network proxy adds the key for api.meshy.ai (saved through the session's secure credential form), and the
files download from assets.meshy.ai. Anywhere else, set MESHY_API_KEY and it is sent as a bearer token.

build_demo.py uses the glbs in OUT (--meshy-dir) in place of its placeholder models, fitting each to the size its physics body needs.
"""

import argparse
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request

API = "https://api.meshy.ai/openapi"

# name -> the prompt, and the number of triangles to ask for (the demo runs on a software renderer, so they are kept low).
ASSETS = {
    "crate": ("a weathered wooden shipping crate with dark iron corner brackets and plank slats, game prop, centered, no ground", 3000),
    "lantern": ("an old black iron hanging oil lantern with a ring at the top and warm glowing amber glass, game prop, centered", 3500),
    "ruin_arch": ("a ruined ancient stone archway overgrown with moss, ivy and small ferns, crumbled blocks at its base, weathered grey granite, "
                  "environment prop for a forest game, centered, no ground plane", 8000),
    "oak_tree": ("a stylized mature oak tree with a thick gnarled trunk and a full rounded leafy green canopy, game-ready environment asset, centered, "
                 "standing on its roots with no ground", 6000),
    "grass_clump": ("a lush clump of tall green meadow grass blades with a few seed heads, game-ready foliage asset, centered, no ground", 1500),
    "wildflowers": ("a small patch of wildflowers, white daisies and purple and yellow blossoms on green stems with leaves, game-ready foliage asset, centered, no ground", 2500),
    "jeep_body": ("a rugged olive-green military off-road jeep body with a canvas-free open cab, windscreen, headlights and roll bar, "
                  "with NO WHEELS and no tires (empty wheel arches), low poly game vehicle, facing forward", 7000),
}


def request(method, path, body=None, retries=3):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"}
    if os.environ.get("MESHY_API_KEY"):
        headers["Authorization"] = "Bearer " + os.environ["MESHY_API_KEY"]
    last = None
    for attempt in range(retries):
        req = urllib.request.Request(API + path, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=60) as response:
                return json.load(response)
        except urllib.error.HTTPError as e:
            text = e.read().decode(errors="replace")
            if e.code in (429, 500, 502, 503, 504) and attempt + 1 < retries:
                time.sleep(5 * (attempt + 1))
                last = f"HTTP {e.code}: {text}"
                continue
            raise SystemExit(f"Meshy {method} {path} failed: HTTP {e.code}: {text}")
        except urllib.error.URLError as e:
            last = str(e)
            time.sleep(3)
    raise SystemExit(f"Meshy {method} {path} failed: {last}")


def wait(path, label, interval=8, timeout=900):
    started = time.time()
    while True:
        task = request("GET", path)
        status = task.get("status")
        print(f"  {label}: {status} {task.get('progress', 0)}%", flush=True)
        if status == "SUCCEEDED":
            return task
        if status in ("FAILED", "CANCELED"):
            raise SystemExit(f"{label} {status}: {json.dumps(task.get('task_error'))}")
        if time.time() - started > timeout:
            raise SystemExit(f"{label} did not finish in {timeout} s")
        time.sleep(interval)


def download(url, target):
    with urllib.request.urlopen(url, timeout=120) as response:
        target.write_bytes(response.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True)
    parser.add_argument("--only", action="append", help="make only this asset (repeatable)")
    parser.add_argument("--balance", action="store_true", help="print the credit balance and stop")
    parser.add_argument("--model", default=None, help="Meshy's ai_model (default: the service's own)")
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    balance = request("GET", "/v1/balance")
    print(f"Meshy credits: {balance.get('balance')}")
    if args.balance:
        return
    state_path = out / "state.json"
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    for name, (prompt, polys) in ASSETS.items():
        if args.only and name not in args.only:
            continue
        glb = out / f"{name}.glb"
        entry = state.setdefault(name, {"prompt": prompt})
        if glb.exists() and entry.get("downloaded"):
            print(f"{name}: done")
            continue
        print(f"{name}: {prompt}")
        if "preview" not in entry:
            body = {"mode": "preview", "prompt": prompt, "art_style": "realistic", "should_remesh": True, "target_polycount": polys, "topology": "triangle"}
            if args.model:
                body["ai_model"] = args.model
            entry["preview"] = request("POST", "/v2/text-to-3d", body)["result"]
            state_path.write_text(json.dumps(state, indent=1))
        wait(f"/v2/text-to-3d/{entry['preview']}", f"{name} preview")
        if "refine" not in entry:
            body = {"mode": "refine", "preview_task_id": entry["preview"], "enable_pbr": True}
            entry["refine"] = request("POST", "/v2/text-to-3d", body)["result"]
            state_path.write_text(json.dumps(state, indent=1))
        task = wait(f"/v2/text-to-3d/{entry['refine']}", f"{name} refine")
        url = (task.get("model_urls") or {}).get("glb")
        if not url:
            raise SystemExit(f"{name}: no glb in {json.dumps(task.get('model_urls'))}")
        download(url, glb)
        entry["downloaded"] = True
        state_path.write_text(json.dumps(state, indent=1))
        print(f"{name}: wrote {glb} ({glb.stat().st_size / 1e6:.1f} MB)")
    print(f"Meshy credits left: {request('GET', '/v1/balance').get('balance')}")


if __name__ == "__main__":
    main()
