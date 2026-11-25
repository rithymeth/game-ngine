#!/usr/bin/env python3
"""Compare the 2D positions from two packaged-player Jump Proof traces."""

import argparse
import json
import math
from pathlib import Path

FRAME_PREFIX = "Jump Trace: "
VERDICT_PREFIX = "Jump Proof: "
MAX_FRAMES = 360_000


def position(record):
    point = record.get("position")
    if (not isinstance(point, list) or len(point) != 3
            or any(type(value) not in (int, float) or not math.isfinite(value) for value in point)):
        raise ValueError("missing or non-finite Player position")
    return point


def read_trace(path):
    points, verdict = [], None
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith(FRAME_PREFIX):
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
    return points


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--tolerance", type=float, default=0.01, help="allowed drift per axis in world units")
    args = parser.parse_args()
    if not math.isfinite(args.tolerance) or args.tolerance < 0:
        parser.error("tolerance must be finite and non-negative")
    try:
        left, right = read_trace(args.before), read_trace(args.after)
        if len(left) != len(right):
            raise ValueError("trace lengths differ")
    except (OSError, ValueError, OverflowError) as exc:
        parser.error(str(exc))

    result = {"frames": len(left), "tolerance": args.tolerance, "first_difference_frame": None}
    for frame, (a, b) in enumerate(zip(left, right), 1):
        if any(abs(a[i] - b[i]) > args.tolerance for i in (0, 1)):
            result.update(first_difference_frame=frame, before=a, after=b)
            break
    print(json.dumps(result, allow_nan=False))
    return 0 if result["first_difference_frame"] is None else 5


if __name__ == "__main__":
    raise SystemExit(main())
