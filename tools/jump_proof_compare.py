#!/usr/bin/env python3
"""Compare the 2D positions from two packaged-player Jump Proof traces."""

import argparse
import json
import math
from pathlib import Path

ROUTE_PREFIX = "Jump Route: "
FRAME_PREFIX = "Jump Trace: "
VERDICT_PREFIX = "Jump Proof: "
MAX_FRAMES = 360_000


def position(record):
    point = record.get("position")
    if (not isinstance(point, list) or len(point) != 3
            or any(type(value) not in (int, float) or not math.isfinite(value) for value in point)):
        raise ValueError("missing or non-finite Player position")
    return point


def finite_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def validate_route(route, frames):
    fields = ("min_x", "max_x", "min_y", "max_y")
    target = route.get("target")
    if (type(route.get("frames")) is not int or route["frames"] != frames
            or not finite_number(route.get("fixed_hz")) or route["fixed_hz"] <= 0
            or not isinstance(route.get("scene"), str) or not route["scene"]
            or not isinstance(target, dict) or not all(finite_number(target.get(key)) for key in fields)
            or target["min_x"] > target["max_x"] or target["min_y"] > target["max_y"]
            or not isinstance(route.get("events"), list)):
        raise ValueError("invalid route descriptor")
    previous = 0
    for event in route["events"]:
        if (not isinstance(event, dict) or type(event.get("frame")) is not int
                or not previous <= event["frame"] < frames
                or not isinstance(event.get("key"), str) or not event["key"]
                or not finite_number(event.get("value"))):
            raise ValueError("invalid input event in route descriptor")
        previous = event["frame"]


def read_trace_details(path):
    points, verdict, route = [], None, None
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith(ROUTE_PREFIX):
                if route is not None or points or verdict is not None:
                    raise ValueError("route descriptor must appear once before trace frames")
                route = json.loads(line[len(ROUTE_PREFIX):])
                if not isinstance(route, dict):
                    raise ValueError("invalid route descriptor")
            elif line.startswith(FRAME_PREFIX):
                sample = json.loads(line[len(FRAME_PREFIX):])
                if (verdict is not None or len(points) == MAX_FRAMES or not isinstance(sample, dict)
                        or type(sample.get("frame")) is not int or sample["frame"] != len(points) + 1):
                    raise ValueError("trace frames must be contiguous and precede the verdict")
                points.append(position(sample))
            elif line.startswith(VERDICT_PREFIX):
                if verdict is not None:
                    raise ValueError("more than one route verdict")
                verdict = json.loads(line[len(VERDICT_PREFIX):])
                if not isinstance(verdict, dict):
                    raise ValueError("invalid route verdict")
    if (not points or verdict is None or type(verdict.get("frames")) is not int
            or verdict["frames"] != len(points) or verdict.get("status") not in ("pass", "missed")
            or position(verdict) != points[-1]):
        raise ValueError("trace does not end with its completed route verdict")
    if route is not None:
        validate_route(route, len(points))
    return points, route, verdict["status"]


def read_trace(path, *, include_route=False):
    points, route, _ = read_trace_details(path)
    return (points, route) if include_route else points


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--overlay-data", action="store_true", help="include both paths for the Tilemap editor")
    parser.add_argument("--tolerance", type=float, default=0.01, help="allowed drift per axis in world units")
    args = parser.parse_args()
    if not math.isfinite(args.tolerance) or args.tolerance < 0:
        parser.error("tolerance must be finite and non-negative")
    try:
        left, left_route, before_status = read_trace_details(args.before)
        right, right_route, after_status = read_trace_details(args.after)
        if left_route != right_route:
            raise ValueError("route descriptors differ (input, target, scene or fixed tick rate)")
        if args.overlay_data and len(left) > 20_000:
            raise ValueError("editor overlay supports up to 20000 frames")
        if len(left) != len(right):
            raise ValueError("trace lengths differ")
    except (OSError, ValueError, OverflowError) as exc:
        parser.error(str(exc))

    result = {"frames": len(left), "tolerance": args.tolerance, "first_difference_frame": None,
              "route_verified": left_route is not None, "before_status": before_status, "after_status": after_status,
              "verdict_changed": before_status != after_status}
    for frame, (a, b) in enumerate(zip(left, right), 1):
        if any(abs(a[i] - b[i]) > args.tolerance for i in (0, 1)):
            result.update(first_difference_frame=frame, before=a, after=b)
            break
    if args.overlay_data:
        result.update(before_path=left, after_path=right)
    print(json.dumps(result, allow_nan=False))
    return 5 if result["verdict_changed"] or result["first_difference_frame"] is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())
