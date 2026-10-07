"""Check actual scene and HUD pixels in the AETHER-01 startup proof capture.

Uses only Python's standard library so the Windows CI runner needs no image
packages. Thresholds cover the existing 960x540 greybox, not arbitrary games.
"""

import argparse
import struct
from pathlib import Path


def verify(path: Path, menu: bool = False, ending: bool = False) -> None:
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
    menu_header = 0
    ending_title = ending_archive = 0
    menu_rows = [0] * 6
    for y in range(height):
        row = y if signed_height < 0 else height - 1 - y
        for x in range(width):
            pixel = offset + row * stride + x * 4
            blue, green, red = data[pixel:pixel + 3]
            if 240 <= x < 740 and 345 <= y < 375 and min(red, green, blue) > 150:
                ending_title += 1
            if 240 <= x < 700 and 310 <= y < 338 and green > 150 and blue > 170 and red < 100:
                ending_archive += 1
            if 260 <= x < 700 and 98 <= y < 130 and min(red, green, blue) > 150:
                menu_header += 1
            if 260 <= x < 690 and green > 150 and blue > 170:
                for index in range(6):
                    if 150 + index * 37 <= y < 174 + index * 37:
                        menu_rows[index] += 1
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
    if menu:
        counts = dict(menu_header=menu_header, **{f"menu_row_{i}": count for i, count in enumerate(menu_rows)})
        minimums = dict(menu_header=300, **{f"menu_row_{i}": 60 for i in range(6)})
    elif ending:
        counts = dict(ending_title=ending_title, ending_archive=ending_archive, hud_cyan=hud_cyan)
        minimums = dict(ending_title=250, ending_archive=150, hud_cyan=800)
    failures = [f"{name}={counts[name]} (minimum {minimum})" for name, minimum in minimums.items()
                if counts[name] < minimum]
    if failures:
        raise ValueError("missing rendered content: " + ", ".join(failures))
    print(f"{path}: {'pause menu' if menu else 'archive ending' if ending else 'scene and HUD'} verified {counts}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--menu", action="store_true", help="check the six-row pause menu")
    mode.add_argument("--ending", action="store_true", help="check the archive ending overlay")
    args = parser.parse_args()
    failed = False
    for path in args.captures:
        try:
            verify(path, args.menu, args.ending)
        except (OSError, ValueError, struct.error) as error:
            print(f"{path}: FAIL: {error}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
