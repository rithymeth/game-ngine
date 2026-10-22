"""Checks an `adb exec-out screencap` raw dump for what the Android app draws.

The app (platforms/android/app/src/main/cpp/native_entry.cpp) clears every frame to (0.15, 0.55, 0.35) and draws an
orange (0.90, 0.45, 0.10) triangle over the middle of the screen with an HLSL pipeline. This samples a patch at the
centre (must be the triangle) and a row near the bottom (must be the clear colour; the status text sits at the top
left). The swap chain may be UNORM or sRGB, so either encoding counts. Passes when at least three quarters of each set
match.

usage: python3 check_screen.py screen.raw
"""
import struct
import sys

CLEAR = (0.15, 0.55, 0.35)
TRIANGLE = (0.90, 0.45, 0.10)
TOLERANCE = 12


def srgb(c):
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def encodings(linear):
    return [tuple(round(255 * c) for c in linear), tuple(round(255 * srgb(c)) for c in linear)]


def score(data, header, width, points, expected):
    matches = 0
    seen = {}
    for x, y in points:
        o = header + (y * width + x) * 4
        rgb = tuple(data[o:o + 3])
        seen[rgb] = seen.get(rgb, 0) + 1
        if any(all(abs(a - b) <= TOLERANCE for a, b in zip(rgb, e)) for e in expected):
            matches += 1
    common = sorted(seen.items(), key=lambda kv: -kv[1])[:3]
    return matches, len(points), common


def main(path):
    data = open(path, "rb").read()
    width, height, _fmt = struct.unpack_from("<III", data, 0)
    # Android 12+ adds a colour-space word after the format; older releases do not.
    header = 16 if len(data) == 16 + width * height * 4 else 12
    if len(data) != header + width * height * 4:
        print(f"unexpected screencap size {len(data)} for {width}x{height}")
        return 1
    centre = [(int(width * (0.5 + dx)), int(height * (0.5 + dy)))
              for dx in (-0.04, 0.0, 0.04) for dy in (-0.04, 0.0, 0.04)]
    bottom = [(int(width * (i + 0.5) / 8), int(height * 0.93)) for i in range(8)]
    ok = True
    for label, points, colour in (("triangle at the centre", centre, TRIANGLE), ("clear colour at the bottom", bottom, CLEAR)):
        matches, total, common = score(data, header, width, points, encodings(colour))
        print(f"{width}x{height} {label}: {matches}/{total} match {encodings(colour)}; most common {common}")
        ok = ok and matches * 4 >= total * 3
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
