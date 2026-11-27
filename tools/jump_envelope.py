#!/usr/bin/env python3
"""Experimental Jump Proof timing sweep for one captured press/release pair."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from jump_proof_compare import position, read_trace_details, validate_route, write_new_json

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


def evidence_fingerprints(player):
    members = {"player": player, "PackageManifest.json": player.parent / "PackageManifest.json",
               "Paks/Game.apak": player.parent / "Paks" / "Game.apak"}
    fingerprints = {}
    for name, path in members.items():
        if not path.is_file():
            raise ValueError(f"{player.name}: missing evidence file {name}")
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        fingerprints[name] = digest.hexdigest()
    return fingerprints


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
    # The packaged player reads event values as float32 and reports those
    # exact effective inputs. Keep every other part of the binding exact.
    expected = {**route, "events": [
        {**event, "value": struct.unpack("<f", struct.pack("<f", event["value"]))[0]}
        for event in route["events"]
    ]}
    if recorded != expected or (result.returncode == 0) != (status == "pass"):
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


def serialize_tilemap_overlay(overlay):
    if not isinstance(overlay, dict) or type(overlay.get("frames")) is not int or not 1 <= overlay["frames"] <= 20_000:
        raise ValueError("invalid Tilemap overlay frame count")
    frames = overlay["frames"]
    marker = overlay.get("first_difference_frame")
    if ("first_difference_frame" not in overlay or
            (marker is not None and (type(marker) is not int or not 1 <= marker <= frames))):
        raise ValueError("invalid Tilemap overlay split frame")
    if (overlay.get("before_status"), overlay.get("after_status"), overlay.get("verdict_changed")) != (
            "pass", "missed", True) or type(overlay.get("verdict_changed")) is not bool:
        raise ValueError("expected a lost-timing Tilemap overlay")
    for key in ("before_path", "after_path"):
        path = overlay.get(key)
        if not isinstance(path, list) or len(path) != frames:
            raise ValueError("expected two complete Tilemap paths")
        for point in path:
            x, y, _ = position({"position": point})
            if abs(x) > 1_000_000 or abs(y) > 1_000_000:
                raise ValueError("position is outside the Tilemap overlay range")
    keys = ("frames", "first_difference_frame", "before_path", "after_path",
            "before_status", "after_status", "verdict_changed")
    text = json.dumps({key: overlay[key] for key in keys}, allow_nan=False, separators=(",", ":"))
    if len(text.encode("utf-8")) + 1 > 4 * 1024 * 1024:
        raise ValueError("Tilemap overlay exceeds 4 MiB")
    return text


def lost_route_evidence(root, trials, lost, include_overlay):
    offset = min(lost, key=lambda value: (abs(value), value))
    before, _, _ = read_trace_details(root / "before" / f"trace-{offset}.txt")
    after, _, _ = read_trace_details(root / "after" / f"trace-{offset}.txt")
    tolerance = 0.01  # same default per-axis tolerance as Jump Proof comparison
    divergence = {"xy_tolerance": tolerance, "first_frame": None}
    for frame, (old, new) in enumerate(zip(before, after), 1):
        if any(abs(old[axis] - new[axis]) > tolerance for axis in (0, 1)):
            divergence.update(first_frame=frame, baseline_position=old, candidate_position=new)
            break
    replay = {"offset": offset, "route": trials[offset], "divergence": divergence}
    if include_overlay:
        overlay = {"frames": len(before), "first_difference_frame": divergence["first_frame"],
                   "before_path": before, "after_path": after, "before_status": "pass",
                   "after_status": "missed", "verdict_changed": True}
        serialize_tilemap_overlay(overlay)
        replay["tilemap_overlay"] = overlay
    return replay


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, required=True, help="trusted packaged player executable")
    parser.add_argument("--candidate", type=Path, help="trusted second package for A/B comparison")
    parser.add_argument("--route", type=Path, required=True, help="recorded Jump Proof route")
    parser.add_argument("--key", required=True, help="canonical input key in the recording, e.g. Space")
    parser.add_argument("--press-frame", type=int, required=True, help="zero-based press frame in the recording")
    parser.add_argument("--radius", type=int, default=6, choices=range(1, MAX_RADIUS + 1))
    parser.add_argument("--require-preserved-offsets", action="store_true",
                        help="exit 5 if the candidate loses any passing baseline offset")
    parser.add_argument("--capture-evidence", action="store_true",
                        help="fingerprint staged files and include a replay for the nearest lost offset")
    parser.add_argument("--include-tilemap-overlay", action="store_true",
                        help="embed both paths for the nearest lost timing")
    parser.add_argument("--output", type=Path, help="write to a new JSON file instead of stdout")
    args = parser.parse_args()
    if args.require_preserved_offsets and args.candidate is None:
        parser.error("--require-preserved-offsets requires --candidate")
    if args.include_tilemap_overlay and (not args.capture_evidence or args.candidate is None):
        parser.error("--include-tilemap-overlay requires --capture-evidence and --candidate")
    try:
        if args.output is not None and args.output.exists():
            raise FileExistsError(f"output file already exists: {args.output}")
        trials, skipped = variants(args.route, args.key, args.press_frame, args.radius)
        if args.require_preserved_offsets and len(trials) < 2:
            raise ValueError("no valid shifted offsets to use as a release gate")
        report = {"schema": "aether.jump-envelope.experimental.v1", "key": args.key,
                  "press_frame": args.press_frame, "radius": args.radius, "skipped_offsets": skipped,
                  "scene": trials[0]["scene"], "fixed_hz": trials[0]["fixed_hz"]}
        players = {"baseline": args.player.resolve(strict=True)}
        if args.candidate is not None:
            players["candidate"] = args.candidate.resolve(strict=True)
        snapshots = ({label: evidence_fingerprints(player) for label, player in players.items()}
                     if args.capture_evidence else {})
        lost_replay = None
        with tempfile.TemporaryDirectory(prefix="aether-jump-envelope-") as scratch:
            root = Path(scratch)
            before = report["baseline"] = sweep(players["baseline"], trials, root / "before")
            if args.require_preserved_offsets and 0 not in before["pass_offsets"]:
                raise ValueError("the original route does not pass on the baseline package")
            if args.candidate is not None:
                after = report["candidate"] = sweep(players["candidate"], trials, root / "after")
                old, new = set(before["pass_offsets"]), set(after["pass_offsets"])
                lost = sorted(old - new)
                report.update(lost_pass_offsets=lost, gained_pass_offsets=sorted(new - old))
                if args.capture_evidence and lost:
                    lost_replay = lost_route_evidence(root, trials, lost, args.include_tilemap_overlay)
        if args.require_preserved_offsets:
            report["gate"] = {"policy": "preserve-pass-offsets", "passed": not report["lost_pass_offsets"]}
        if args.capture_evidence:
            for label, player in players.items():
                if evidence_fingerprints(player) != snapshots[label]:
                    raise ValueError(f"{player.name}: package files changed during the sweep")
            evidence = report["evidence"] = {f"{label}_sha256": files for label, files in snapshots.items()}
            evidence["reference_route"] = trials[0]
            if lost_replay is not None:
                evidence["nearest_lost_replay"] = lost_replay
        receipt = json.dumps(report, allow_nan=False)
        if args.output is None:
            print(receipt)
        else:
            write_new_json(args.output, receipt)
        return 5 if args.require_preserved_offsets and not report["gate"]["passed"] else 0
    except (OSError, ValueError, OverflowError, subprocess.TimeoutExpired) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
