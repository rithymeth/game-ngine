"""Check actual scene and HUD pixels in the AETHER-01 startup proof capture.

Uses only Python's standard library so the Windows CI runner needs no image
packages. Thresholds cover the existing 960x540 greybox, not arbitrary games.
"""

import argparse
import struct
from pathlib import Path


def verify(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a BMP capture")
    offset = struct.unpack_from("<I", data, 10)[0]
    dib_size, width, signed_height, planes, bits, compression = struct.unpack_from("<IiiHHI", data, 14)
    height = abs(signed_height)
    if dib_size < 40 or (width, height, planes, bits, compression) != (960, 540, 1, 32, 0):
        raise ValueError("expected an uncompressed 960x540 32-bit BMP")
    stride = width * 4
    if offset < 14 + dib_size or offset + stride * height > len(data):
        raise ValueError("truncated BMP pixels")
    scene_red = hud_cyan = hud_text = player_white = 0
    for y in range(height):
        row = y if signed_height < 0 else height - 1 - y
        for x in range(width):
            pixel = offset + row * stride + x * 4
            blue, green, red = data[pixel:pixel + 3]
            if y > 130 and red > 150 and green < 100 and blue < 100:
                scene_red += 1
            if x < 280 and y < 130 and blue > 170 and green > 150 and red < 100:
                hud_cyan += 1
            if 24 <= x < 936 and 22 <= y < 90 and min(red, green, blue) > 150:
                hud_text += 1
            if 400 < x < 560 and 300 < y < 440 and min(red, green, blue) > 240:
                player_white += 1
    counts = dict(scene_red=scene_red, hud_cyan=hud_cyan, hud_text=hud_text, player_white=player_white)
    minimums = dict(scene_red=40000, hud_cyan=1200, hud_text=500, player_white=1000)
    failures = [f"{name}={counts[name]} (minimum {minimum})" for name, minimum in minimums.items()
                if counts[name] < minimum]
    if failures:
        raise ValueError("missing rendered content: " + ", ".join(failures))
    print(f"{path}: scene and HUD verified {counts}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    args = parser.parse_args()
    failed = False
    for path in args.captures:
        try:
            verify(path)
        except (OSError, ValueError, struct.error) as error:
            print(f"{path}: FAIL: {error}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
