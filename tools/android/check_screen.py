"""Checks an `adb exec-out screencap` raw dump for the colour the Android app clears to.

The app (platforms/android/app/src/main/cpp/native_entry.cpp) clears every frame to (0.15, 0.55, 0.35). The swap
chain may be UNORM or sRGB, so either encoding counts. Samples a grid over the lower two thirds of the screen (the
status text sits at the top left) and passes when most samples match.

usage: python3 check_screen.py screen.raw
"""
import struct
import sys

LINEAR = (0.15, 0.55, 0.35)


def srgb(c):
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


EXPECTED = [tuple(round(255 * c) for c in LINEAR), tuple(round(255 * srgb(c)) for c in LINEAR)]
TOLERANCE = 12


def main(path):
    data = open(path, "rb").read()
    width, height, _fmt = struct.unpack_from("<III", data, 0)
    # Android 12+ adds a colour-space word after the format; older releases do not.
    header = 16 if len(data) == 16 + width * height * 4 else 12
    if len(data) != header + width * height * 4:
        print(f"unexpected screencap size {len(data)} for {width}x{height}")
        return 1
    matches = total = 0
    seen = {}
    for gy in range(6):
        for gx in range(6):
            x = int((gx + 0.5) * width / 6)
            y = int(height / 3 + (gy + 0.5) * (2 * height / 3) / 6)
            o = header + (y * width + x) * 4
            rgb = tuple(data[o:o + 3])
            seen[rgb] = seen.get(rgb, 0) + 1
            total += 1
            if any(all(abs(a - b) <= TOLERANCE for a, b in zip(rgb, e)) for e in EXPECTED):
                matches += 1
    common = sorted(seen.items(), key=lambda kv: -kv[1])[:3]
    print(f"{width}x{height}: {matches}/{total} samples match {EXPECTED}; most common {common}")
    return 0 if matches * 4 >= total * 3 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
