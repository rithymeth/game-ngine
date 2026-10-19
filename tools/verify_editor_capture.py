"""Validate an editor screenshot artifact and write its CI provenance manifest."""

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def inspect_capture(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG capture")

    offset = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    saw_end = False
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 12 + length
        if end > len(data):
            raise ValueError("truncated PNG chunk")
        payload = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != expected_crc:
            raise ValueError(f"bad {kind.decode('ascii', errors='replace')} chunk checksum")
        if kind == b"IHDR":
            if width is not None or length != 13:
                raise ValueError("invalid PNG header")
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if compression != 0 or filtering != 0:
                raise ValueError("unsupported PNG compression or filter method")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            if length != 0:
                raise ValueError("invalid PNG end chunk")
            saw_end = True
            offset = end
            break
        offset = end

    if not saw_end or offset != len(data):
        raise ValueError("missing PNG end chunk or trailing bytes")
    if width is None or height is None or width < 320 or height < 200:
        raise ValueError("capture is smaller than 320x200")
    if (bit_depth, color_type, interlace) != (8, 6, 0):
        raise ValueError("expected a non-interlaced 8-bit RGBA PNG")

    try:
        pixels = zlib.decompress(compressed)
    except zlib.error as error:
        raise ValueError(f"invalid PNG image data: {error}") from error
    stride = width * 4
    if len(pixels) != height * (stride + 1):
        raise ValueError("PNG pixel data size does not match its dimensions")
    if any(pixels[row * (stride + 1)] > 4 for row in range(height)):
        raise ValueError("PNG contains an invalid row filter")

    return {
        "width": width,
        "height": height,
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--optional", action="store_true", help="allow a runner without a capture device")
    args = parser.parse_args()

    manifest: dict[str, object] = {"platform": args.platform, "capture": str(args.capture), "status": "missing"}
    try:
        details = inspect_capture(args.capture)
        manifest.update(status="verified", **details)
        print(f"{args.capture}: verified {details['width']}x{details['height']} PNG ({details['sha256']})")
    except FileNotFoundError:
        if not args.optional:
            manifest["status"] = "missing-required"
            args.manifest.parent.mkdir(parents=True, exist_ok=True)
            args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            print(f"{args.capture}: required screenshot is missing")
            return 1
        manifest["reason"] = "runner did not produce a screenshot"
        print(f"{args.capture}: no screenshot available on this runner")
    except (OSError, ValueError, struct.error) as error:
        manifest.update(status="invalid", reason=str(error))
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"{args.capture}: FAIL: {error}")
        return 1

    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
