#!/usr/bin/env python3
"""Experimental Jump Proof timing sweep for one captured press/release pair."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from jump_proof_compare import read_trace_details, validate_route, write_new_json

MAX_ROUTE_BYTES = 2 * 1024 * 1024
MAX_RADIUS = 12


def variants(path, key, press_frame, radius):
    with path.open("rb") as source:
        data = source.read(MAX_ROUTE_BYTES + 1)
    if len(data) > MAX_ROUTE_BYTES:
        raise ValueError("recorded route exceeds 2 MiB")
    route = json.loads(data.decode("utf-8-sig"))
    if not isinstance(route, dict) or type(route.get("frames")) is not int or not 1 <= route["frames"] <= 20_000:
        raise ValueError("expected a recorded route with 1 to 20000 frames")
    validate_route(route, route["frames"])
    route = {field: route[field] for field in ("frames", "fixed_hz", "scene", "target", "events")}
    route["target"] = {field: route["target"][field] for field in ("min_x", "max_x", "min_y", "max_y")}
    events = [{field: event[field] for field in ("frame", "key", "value")} for event in route["events"]]
    same_key = [i for i, event in enumerate(events) if event["key"] == key]
    matches = [n for n, i in enumerate(same_key) if events[i]["frame"] == press_frame]
    if len(matches) != 1:
        raise ValueError("select exactly one event by canonical key name and press frame")
    n = matches[0]
    if n + 1 == len(same_key):
        raise ValueError("selected press has no recorded release")
    p, r = same_key[n:n + 2]
    release = events[r]["frame"]
    if (events[p]["value"] != 1 or events[r]["value"] != 0 or press_frame >= release
            or (n and events[same_key[n - 1]]["value"] != 0)
            or (n + 2 < len(same_key) and events[same_key[n + 2]]["value"] != 1)):
        raise ValueError("selected events must form an isolated digital press/release pair")
    lower = events[same_key[n - 1]]["frame"] + 1 if n else 0
    upper = events[same_key[n + 2]]["frame"] - 1 if n + 2 < len(same_key) else route["frames"] - 1
    trials, skipped = {}, []
    for offset in range(-radius, radius + 1):
        if press_frame + offset < lower or release + offset > upper:
            skipped.append(offset)
            continue
        shifted = [{**event, "frame": event["frame"] + (offset if i in (p, r) else 0)}
                   for i, event in enumerate(events)]
        trials[offset] = {**route, "events": sorted(shifted, key=lambda event: event["frame"])}
    if 0 not in trials:
        raise ValueError("unshifted pulse overlaps another event for the same key")
    return trials, skipped


def run_trial(player, route, root, label):
    route_path, trace_path = root / f"route-{label}.json", root / f"trace-{label}.txt"
    if not route_path.exists():
        route_path.write_text(json.dumps(route, allow_nan=False), encoding="utf-8")
    with trace_path.open("wb") as stdout:
        result = subprocess.run([player, "--jump-proof", route_path, "--jump-trace",
                                 "--user-dir", root / f"profile-{label}"],
                                cwd=root, stdout=stdout, stderr=subprocess.PIPE, timeout=30, check=False)
    if result.returncode not in (0, 5):
        raise ValueError(f"{player.name}, trial {label}: player exited {result.returncode}")
    points, recorded, status = read_trace_details(trace_path)
    if recorded != route or (result.returncode == 0) != (status == "pass"):
        raise ValueError(f"{player.name}, trial {label}: replay trace does not match the requested variant")
    return points, status


def sweep(player, trials, root):
    root.mkdir()
    control, reference = run_trial(player, trials[0], root, "0")
    outcomes = {0: {"status": reference, "landing": control[-1]}}
    for offset, route in trials.items():
        if offset:
            points, status = run_trial(player, route, root, str(offset))
            outcomes[offset] = {"status": status, "landing": points[-1]}
    repeat, repeat_status = run_trial(player, trials[0], root, "repeat")
    if control != repeat or reference != repeat_status:
        raise ValueError(f"{player.name}: reference replay was not repeatable")
    passes = {offset for offset, result in outcomes.items() if result["status"] == "pass"}
    window = None
    if 0 in passes:
        lo = hi = 0
        while lo - 1 in passes:
            lo -= 1
        while hi + 1 in passes:
            hi += 1
        window = [lo, hi]
    return {"player": str(player), "pass_offsets": sorted(passes), "contiguous_pass_offsets": window,
            "trials": [{"offset": offset, **outcomes[offset]} for offset in trials]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, required=True, help="trusted packaged player executable")
    parser.add_argument("--candidate", type=Path, help="trusted second package for A/B comparison")
    parser.add_argument("--route", type=Path, required=True, help="recorded Jump Proof route")
    parser.add_argument("--key", required=True, help="canonical input key in the recording, e.g. Space")
    parser.add_argument("--press-frame", type=int, required=True, help="zero-based press frame in the recording")
    parser.add_argument("--radius", type=int, default=6, choices=range(1, MAX_RADIUS + 1))
    parser.add_argument("--output", type=Path, help="write to a new JSON file instead of stdout")
    args = parser.parse_args()
    try:
        if args.output is not None and args.output.exists():
            raise FileExistsError(f"output file already exists: {args.output}")
        trials, skipped = variants(args.route, args.key, args.press_frame, args.radius)
        report = {"schema": "aether.jump-envelope.experimental.v1", "key": args.key,
                  "press_frame": args.press_frame, "radius": args.radius, "skipped_offsets": skipped,
                  "scene": trials[0]["scene"], "fixed_hz": trials[0]["fixed_hz"]}
        with tempfile.TemporaryDirectory(prefix="aether-jump-envelope-") as scratch:
            root = Path(scratch)
            before = report["baseline"] = sweep(args.player.resolve(strict=True), trials, root / "before")
            if args.candidate is not None:
                after = report["candidate"] = sweep(args.candidate.resolve(strict=True), trials, root / "after")
                old, new = set(before["pass_offsets"]), set(after["pass_offsets"])
                report.update(lost_pass_offsets=sorted(old - new), gained_pass_offsets=sorted(new - old))
        receipt = json.dumps(report, allow_nan=False)
        if args.output is None:
            print(receipt)
        else:
            write_new_json(args.output, receipt)
    except (OSError, ValueError, OverflowError, subprocess.TimeoutExpired) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
